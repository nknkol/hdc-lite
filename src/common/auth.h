/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
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
#ifndef HDC_AUTH_H
#define HDC_AUTH_H
#include "common.h"

// If these functions occupy too high a load, you can consider placing them in a thread for execution, and perform
// subsequent processing in the callback directly after completion.
namespace HdcAuth {
const uint8_t RSA_TOKEN_SIZE = 20;  // SHA_DIGEST_LENGTH
// in host out==RSA*, in daemon out=RSAPublicKey*
bool KeylistIncrement(list<void *> *listKey, uint8_t &authKeyIndex, void **out);
void FreeKey(bool publicOrPrivate, list<void *> *listKey);

#ifdef HDC_HOST
bool RsaSignAndBase64(string &buf, Hdc::AuthVerifyType type);
bool GetPublicKeyinfo(string &pubkey_info);
int RsaPrikeyDecryptPsk(const unsigned char* in, int inLen, unsigned char* out, int outBufSize);
#else
int RsaPubkeyEncryptPsk(const unsigned char* in, int inLen, unsigned char* out, int outBufSize, const string& pubkey);
#endif

// host
bool GenerateKey(const char *file);
int AuthSign(void *rsa, const unsigned char *token, size_t tokenSize, void *sig);
int GetPublicKeyFileBuf(unsigned char *data, size_t len);

// daemon
bool AuthVerify(uint8_t *token, uint8_t *sig, int siglen);
bool PostUIConfirm(string publicKey);

#ifdef HDC_SUPPORT_ENCRYPT_PRIVATE_KEY
#define HDC_PRIVATE_KEY_FILE_PWD_KEY_ALIAS "hdc_private_key_file_pwd_key_alias"
const std::string HDC_PRIVATE_KEY_FILE_FIRST_LINE_STR = "-----BEGIN PRIVATE KEY-----";
bool CheckPrivateKeyFile();
#endif
}

#endif