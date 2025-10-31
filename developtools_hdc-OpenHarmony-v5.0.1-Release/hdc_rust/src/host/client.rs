/*
 * Copyright (C) 2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
use super::parser::ParsedCommand;
use super::server;

use hdc::common::base;
use hdc::common::base::Base;
use hdc::config::{self, HdcCommand};
use hdc::transfer;
use hdc::utils;
#[allow(unused)]
use hdc::utils::hdc_log::*;
use libc::exit;
use std::time::Duration;

use std::env;
use std::io::{self, Error, ErrorKind, Write};
#[cfg(not(target_os = "windows"))]
use std::os::fd::AsRawFd;

#[cfg(featrue = "host")]
extern crate ylong_runtime_static as ylong_runtime;
#[cfg(not(target_os = "windows"))]
use ylong_runtime::io::AsyncReadExt;
use ylong_runtime::io::AsyncWriteExt;
use ylong_runtime::net::{SplitWriteHalf, TcpStream};

#[cfg(target_os = "windows")]
use crate::tty_utility::*;

#[cfg(target_os = "windows")]
extern "C" {
    fn getch() -> libc::c_int;
}


#[allow(unused)]
pub struct Client {
    command: HdcCommand,
    params: Vec<String>,
    connect_key: String,
    wr: SplitWriteHalf,
}

pub async fn run_client_mode(parsed_cmd: ParsedCommand) -> io::Result<()> {
    match parsed_cmd.command {
        Some(HdcCommand::KernelServerStart) => {
            if parsed_cmd.parameters.contains(&"-r".to_string()) {
                server::server_kill().await;
            }
            server::server_fork(parsed_cmd.server_addr.clone(), parsed_cmd.log_level).await;
            return Ok(());
        }
        Some(HdcCommand::KernelServerKill) => {
            server::server_kill().await;
            if parsed_cmd.parameters.contains(&"-r".to_string()) {
                server::server_fork(parsed_cmd.server_addr.clone(), parsed_cmd.log_level).await;
            }
            return Ok(());
        }
        _ => {}
    };

    if parsed_cmd.launch_server && Base::program_mutex(base::GLOBAL_SERVER_NAME, true) {
        server::server_fork(parsed_cmd.server_addr.clone(), parsed_cmd.log_level).await;
    }

    // TODO: other cmd before initial client

    let mut client = Client::new(parsed_cmd).await?;

    if let Err(e) = client.handshake().await {
        hdc::error!("handshake with server failed: {e:?}");
        return Err(e);
    }
    client.execute_command().await
}

impl Client {
    pub async fn new(parsed_cmd: ParsedCommand) -> io::Result<Self> {
        let Some(command) = parsed_cmd.command else {
            return Err(Error::new(ErrorKind::Other, "command is None"));
        };
        let connect_key = auto_connect_key(parsed_cmd.connect_key, command);

        let stream = match TcpStream::connect(parsed_cmd.server_addr).await {
            Ok(stream) => stream,
            Err(_) => return Err(Error::new(ErrorKind::Other, "Connect to server failed")),
        };

        let (rd, wr) = stream.into_split();

        transfer::ChannelMap::start(rd).await;

        Ok(Self {
            command,
            params: parsed_cmd.parameters,
            connect_key,
            wr,
        })
    }

    async fn execute_command(&mut self) -> io::Result<()> {
        let entire_cmd = self.params.join(" ");
        hdc::debug!("execute command params: {}", &entire_cmd);

        match self.command {
            HdcCommand::KernelTargetList
            | HdcCommand::KernelTargetConnect
            | HdcCommand::UnityHilog => self.general_task().await,
            HdcCommand::FileInit | HdcCommand::FileCheck | HdcCommand::FileRecvInit => {
                self.file_send_task().await
            }
            HdcCommand::AppInit => self.app_install_task().await,
            HdcCommand::AppUninstall => self.app_uninstall_task().await,
            HdcCommand::UnityRunmode
            | HdcCommand::UnityReboot
            | HdcCommand::UnityRemount => self.unity_task().await,
            HdcCommand::UnityRootrun => self.unity_root_run_task().await,
            HdcCommand::UnityExecute => self.shell_task().await,
            HdcCommand::KernelWaitFor => self.wait_task().await,
            HdcCommand::UnityBugreportInit => self.bug_report_task().await,
            HdcCommand::ForwardInit
            | HdcCommand::ForwardRportInit
            | HdcCommand::ForwardList
            | HdcCommand::ForwardRportList
            | HdcCommand::ForwardRemove
            | HdcCommand::ForwardRportRemove => {
                self.forward_task().await
            }
            HdcCommand::JdwpList | HdcCommand::JdwpTrack => self.jdwp_task().await,
            HdcCommand::KernelCheckServer => self.check_server_task().await,
            _ => Err(Error::new(
                ErrorKind::Other,
                format!("unknown command: {}", self.command as u32),
            )),
        }
    }

    pub async fn handshake(&mut self) -> io::Result<()> {
        let recv = self.recv().await?;
        let msg = match std::str::from_utf8(&recv[..config::HANDSHAKE_MESSAGE.len()]) {
            Ok(msg) => msg,
            Err(err) => {
                return Err(Error::new(ErrorKind::Other, format!("handshake from_utf8 error : {err}")));
            }
        };
        if msg != config::HANDSHAKE_MESSAGE {
            return Err(Error::new(ErrorKind::Other, "Recv server-hello failed"));
        }
        let buf = [
            config::HANDSHAKE_MESSAGE.as_bytes(),
            vec![0_u8; config::BANNER_SIZE - config::HANDSHAKE_MESSAGE.len()].as_slice(),
            self.connect_key.as_bytes(),
            vec![0_u8; config::KEY_MAX_SIZE - self.connect_key.len()].as_slice(),
        ]
        .concat();

        self.send(buf.as_slice()).await;

        Ok(())
    }

    async fn send(&mut self, buf: &[u8]) {
        hdc::debug!("channel send buf: {:#?}", buf);
        let msg = [u32::to_be_bytes(buf.len() as u32).as_slice(), buf].concat();
        let _ = self.wr.write_all(msg.as_slice()).await;
    }

    async fn recv(&mut self) -> io::Result<Vec<u8>> {
        hdc::debug!("channel recv buf");
        transfer::ChannelMap::recv().await
    }

    async fn unity_task(&mut self) -> io::Result<()> {
        self.send(self.params.join(" ").as_bytes()).await;
        self.loop_recv().await
    }

    async fn wait_task(&mut self) -> io::Result<()> {
        self.send(self.params.join(" ").as_bytes()).await;
        self.loop_recv_waitfor().await
    }

    async fn unity_root_run_task(&mut self) -> io::Result<()> {
        if self.params.len() >= 2 && self.params[1].starts_with("-r") {
            self.params[1] = "r".to_string();
        }
        self.send(self.params.join(" ").as_bytes()).await;
        self.loop_recv().await
    }
    async fn jdwp_task(&mut self) -> io::Result<()> {
        self.send(self.params.join(" ").as_bytes()).await;
        self.loop_recv().await
    }


    #[cfg(target_os = "windows")]
    async fn  shell_task(&mut self) -> io::Result<()> {
        let cmd = match self.params.len() {
            1 => "shell\0".to_string(),
            _ => self.params.join(" "),
        };

        self.send(cmd.as_bytes()).await;

        let _handle = ylong_runtime::spawn(async move {
            loop {
                match transfer::ChannelMap::recv().await {
                    Ok(recv) => {
                        let _ = utils::print_msg(recv).await;
                    }
                    Err(_) => {
                        std::process::exit(0);
                    }
                }
            }
        });

        loop {
            let c = unsafe { getch() };

            // 判断如果是ctrl_D,发送给serer端，client直接退出
            if c == 0x4 {
                self.send([c as u8].as_slice()).await;
                break;
            }

            // win下的控制字符以0xe0开头，转换后发送给server，
            if c == 0xe0 {
                let control_code = convert_to_control_code();
                self.send(control_code.as_slice()).await;
                continue;
            }

            let unicode_byte = unicode_assemble(c);
            hdc::info!("unicode_byte is {:?}", unicode_byte);
            self.send(unicode_byte.as_slice()).await;
        }
        Ok(())
    }

    #[cfg(not(target_os = "windows"))]
    async fn shell_task(&mut self) -> io::Result<()> {
        let cmd = match self.params.len() {
            1 => "shell\0".to_string(),
            _ => self.params.join(" "),
        };

        self.send(cmd.as_bytes()).await;

        let termios = setup_raw_terminal()?;
        let termios_clone = termios;

        let _handle = ylong_runtime::spawn(async move {
            loop {
                match transfer::ChannelMap::recv().await {
                    Ok(recv) => {
                        let _ = utils::print_msg(recv).await;
                    }
                    Err(_) => {
                        let _ = recover_terminal(termios_clone);

                        std::process::exit(0);
                    }
                }
            }
        });

        let mut buf = [0_u8; 1];
        let mut stdin = ylong_runtime::io::stdin();

        while let Ok(bytes) = stdin.read(&mut buf).await {
            self.send(&buf[..bytes]).await;
            if buf[..bytes].contains(&0x4_u8) {
                break;
            }
        }

        let _ = recover_terminal(termios);
        Ok(())
    }

    async fn forward_task(&mut self) -> io::Result<()> {
        if (self.command == HdcCommand::ForwardRemove
            || self.command == HdcCommand::ForwardRportRemove)
            && self.params.len() < 3
        {
            return Err(Error::new(
                ErrorKind::Other,
                "Too few arguments.".to_string()
            ));
        }
        if (self.command == HdcCommand::ForwardInit
            || self.command == HdcCommand::ForwardRportInit)
            && self.params.len() < 3
        {
            return Err(Error::new(
                ErrorKind::Other,
                "Too few arguments.".to_string()
            ));
        }
        self.send(self.params.join(" ").as_bytes()).await;
        self.loop_recv().await
    }

    async fn general_task(&mut self) -> io::Result<()> {
        self.send(self.params.join(" ").as_bytes()).await;
        loop {
            let recv = self.recv().await?;
            hdc::debug!(
                "general_task recv: {:#?}",
                recv.iter()
                    .map(|c| format!("{c:02x}"))
                    .collect::<Vec<_>>()
                    .join(" ")
            );
            let _ = utils::print_msg(recv).await;
        }
    }

    async fn bug_report_task(&mut self) -> io::Result<()> {
        if self.params.len() <= 1 {
            return self.general_task().await;
        }
        self.send(self.params.join(" ").as_bytes()).await;
        let mut file = std::fs::File::create(self.params[1].as_str())?;
        loop {
            let recv = self.recv().await?;
            file.write_all(&recv)?;
        }
    }

    async fn file_send_task(&mut self) -> io::Result<()> {
        let mut params = self.params.clone();
        if self.command == HdcCommand::FileInit || self.command == HdcCommand::FileRecvInit {
            let command_field_count = 2;
            let current_dir = env::current_dir()?;
            let mut s = current_dir.display().to_string();
            s.push(Base::get_path_sep());
            params.insert(command_field_count, "-cwd".to_string());
            params.insert(command_field_count + 1, s.clone());
        }

        self.send(params.join(" ").as_bytes()).await;
        self.loop_recv().await
    }

    async fn loop_recv(&mut self) -> io::Result<()> {
        loop {
            let recv = self.recv().await;
            match recv {
                Ok(recv) => {
                    hdc::debug!(
                        "recv: {:#?}",
                        recv.iter()
                            .map(|c| format!("{c:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                    match String::from_utf8(recv) {
                        Ok(msg) => print!("{msg}"),
                        Err(err) => return Err(Error::new(ErrorKind::Other, format!("recv data to str failed, {err}"))),
                    }
                }
                Err(e) => {
                    return Err(e);
                }
            }
        }
    }

    async fn loop_recv_waitfor(&mut self) -> io::Result<()> {
        loop {
            let recv = self.recv().await;
            match recv {
                Ok(recv) => {
                    hdc::debug!(
                        "recv: {:#?}",
                        recv.iter()
                            .map(|c| format!("{c:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                    if let HdcCommand::KernelWaitFor = self.command {
                        let wait_for = "No connected target\r\n".to_string();
                        if wait_for == String::from_utf8(recv).expect("invalid UTF-8") {
                            self.send(self.params.join(" ").as_bytes()).await;
                            hdc::debug!("WaitFor sleep a second");
                            let wait_interval = 1000;
                            ylong_runtime::time::sleep(Duration::from_millis(wait_interval)).await;
                        } else {
                            hdc::debug!("exit client");
                            unsafe {
                                exit(0);
                            }
                        }
                    }
                }
                Err(e) => {
                    return Err(e);
                }
            }
        }
    }

    async fn app_install_task(&mut self) -> io::Result<()> {
        let mut params = self.params.clone();
        let command_field_count = 1;
        let current_dir = env::current_dir()?;
        let mut s = current_dir.display().to_string();
        s.push(Base::get_path_sep());
        params.insert(command_field_count, "-cwd".to_string());
        params.insert(command_field_count + 1, s.clone());

        self.send(params.join(" ").as_bytes()).await;

        loop {
            let recv = self.recv().await;
            match recv {
                Ok(recv) => {
                    hdc::debug!(
                        "app_install_task recv: {:#?}",
                        recv.iter()
                            .map(|c| format!("{c:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                    match String::from_utf8(recv) {
                        Ok(msg) => print!("{}", msg),
                        Err(e) => return Err(io::Error::new(io::ErrorKind::Other, format!("{e}"))),
                    }
                }
                Err(e) => {
                    return Err(e);
                }
            }
        }
    }

    async fn app_uninstall_task(&mut self) -> io::Result<()> {
        let params = self.params.clone();
        self.send(params.join(" ").as_bytes()).await;

        loop {
            let recv = self.recv().await;
            match recv {
                Ok(recv) => {
                    hdc::debug!(
                        "app_uninstall_task recv: {:#?}",
                        recv.iter()
                            .map(|c| format!("{c:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                    match String::from_utf8(recv) {
                        Ok(msg) => println!("{msg}"),
                        Err(e) => {
                            return Err(io::Error::new(io::ErrorKind::Other, format!("{e}")));
                        }
                    }
                }
                Err(e) => {
                    return Err(e);
                }
            }
        }
    }

    async fn check_server_task(&mut self) -> io::Result<()> {
        let params = self.params.clone();
        self.send(params.join(" ").as_bytes()).await;

        let recv = self.recv().await;
        match recv {
            Ok(recv) => {
                hdc::debug!(
                    "check_server_task recv: {:#?}",
                    recv.iter()
                        .map(|c| format!("{c:02x}"))
                        .collect::<Vec<_>>()
                        .join(" ")
                );

                const CMD_U8_LEN: usize = 2;
                if recv.len() < CMD_U8_LEN {
                    return Err(Error::new(io::ErrorKind::Other, "recv failed"));
                }

                let (cmd_slice, version_slice) = recv.split_at(CMD_U8_LEN);
                let Ok(cmd) = HdcCommand::try_from(u16::from_le_bytes(cmd_slice.try_into().unwrap_or_default()) as u32)
                else {
                    return Err(Error::new(io::ErrorKind::Other, "HdcCommand::try_from failed"));
                };
                if HdcCommand::KernelCheckServer != cmd {
                    return Err(Error::new(io::ErrorKind::Other, "recv cmd error"));
                }
                match String::from_utf8(version_slice.to_vec()) {
                    Ok(s_ver) => println!("Client version:{}, server version:{}", config::get_version(), s_ver),
                    Err(err) => {
                        return Err(Error::new(io::ErrorKind::Other, format!("from_utf8 failed, {err}")));
                    }
                }
                Ok(())
            }
            Err(e) => Err(e),
        }
    }
}

fn auto_connect_key(key: String, cmd: HdcCommand) -> String {
    match cmd {
        HdcCommand::ClientVersion
        | HdcCommand::KernelHelp
        | HdcCommand::KernelTargetDiscover
        | HdcCommand::KernelTargetList
        | HdcCommand::KernelCheckServer
        | HdcCommand::KernelTargetConnect
        | HdcCommand::KernelCheckDevice
        | HdcCommand::KernelServerKill => "".to_string(),
        _ => {
            if key.is_empty() {
                "any".to_string()
            } else {
                key
            }
        }
    }
}

#[cfg(not(target_os = "windows"))]
fn setup_raw_terminal() -> io::Result<libc::termios> {
    unsafe {
        let tty;
        let fd = if libc::isatty(libc::STDIN_FILENO) == 1 {
            libc::STDIN_FILENO
        } else {
            tty = std::fs::File::open("/dev/tty")?;
            tty.as_raw_fd()
        };

        let mut ptr = core::mem::MaybeUninit::uninit();

        if libc::tcgetattr(fd, ptr.as_mut_ptr()) == 0 {
            let termios = ptr.assume_init();
            let mut termios_copy = termios;
            let c_oflag = termios.c_oflag;
            libc::cfmakeraw(&mut termios_copy);
            termios_copy.c_oflag = c_oflag;

            if libc::tcsetattr(fd, libc::TCSADRAIN, &termios_copy) == 0 {
                return Ok(termios);
            }
        }
    }

    Err(io::Error::last_os_error())
}

#[cfg(not(target_os = "windows"))]
fn recover_terminal(termios: libc::termios) -> io::Result<()> {
    unsafe {
        let tty;
        let fd = if libc::isatty(libc::STDIN_FILENO) == 1 {
            libc::STDIN_FILENO
        } else {
            tty = std::fs::File::open("/dev/tty")?;
            tty.as_raw_fd()
        };
        if libc::tcsetattr(fd, libc::TCSADRAIN, &termios) == 0 {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        }
    }
}
