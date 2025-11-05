#!/bin/bash

# 遇到错误立即退出
set -e

# --- 脚本配置 ---
SOURCE_URL="https://gitee.com/openharmony/developtools_hdc/repository/archive/OpenHarmony-v5.0.1-Release.zip"
ZIP_FILE="OpenHarmony-v5.0.1-Release.zip"
EXTRACTED_DIR_NAME="developtools_hdc-OpenHarmony-v5.0.1-Release"
ORIG_SRC_DIR="hdc"         # 原始代码 (将被下载和覆盖)
MODIFIED_SRC_DIR="src"     # 您修改过的代码 (用于对比)
PATCH_FILE="hdc-lite.patch"  # 目标补丁文件 (将被更新)


# --- 辅助函数：检查工具是否存在 ---
check_tool() {
    if ! command -v $1 &> /dev/null; then
        echo "========================================="
        echo " 错误：依赖工具 '$1' 未安装。"
        echo " 请先安装 $1 后再试！"
        echo "=========================================" >&2
        exit 1
    fi
}

# --- 检查依赖工具 ---
check_tool "unzip"
check_tool "git" # git diff 需要 git
# 检查 wget 或 curl 是否存在
if ! command -v wget &> /dev/null && ! command -v curl &> /dev/null; then
    echo "========================================="
    echo " 错误：依赖工具 'wget' 或 'curl' 未安装。"
    echo " 请至少安装其中一个用于下载。"
    echo "=========================================" >&2
    exit 1
fi

echo "========================================="
echo "开始更新补丁文件: ${PATCH_FILE}"
echo "========================================="

# 1. 检查您修改的 'src' 目录是否存在
if [ ! -d "${MODIFIED_SRC_DIR}" ]; then
    echo " 错误：未找到您修改过的 '${MODIFIED_SRC_DIR}' 目录。"
    echo " 无法生成补丁。"
    exit 1
fi

# 2. 清理旧的原始代码和 zip
echo "  1. 清理旧的原始代码目录 ('${ORIG_SRC_DIR}') 和 zip 文件..."
rm -rf "${ORIG_SRC_DIR}"
rm -f "${ZIP_FILE}"

# 3. 下载新的原始代码
echo "  2. 正在下载新的原始代码: ${SOURCE_URL}"
if command -v wget &> /dev/null; then
    wget -q -O "${ZIP_FILE}" "${SOURCE_URL}"
else
    curl -s -L -o "${ZIP_FILE}" "${SOURCE_URL}"
fi

# 4. 解压
echo "  3. 解压原始代码..."
unzip -q "${ZIP_FILE}"

# 5. 重命名
echo "  4. 重命名为 '${ORIG_SRC_DIR}'..."
mv "${EXTRACTED_DIR_NAME}" "${ORIG_SRC_DIR}"

# 6. 核心步骤：重新生成补丁文件
echo "  5. 正在对比 '${ORIG_SRC_DIR}/src' 和 '${MODIFIED_SRC_DIR}'..."
git diff --no-index --color=never "${ORIG_SRC_DIR}/src" "${MODIFIED_SRC_DIR}" > "${PATCH_FILE}"

# 7. 清理下载的 zip
rm -f "${ZIP_FILE}"

echo ""
echo "========================================="
echo "✓ 补丁文件更新成功!"
echo "  -> ${PATCH_FILE}"
echo "========================================="