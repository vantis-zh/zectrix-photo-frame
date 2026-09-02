#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 加载 .env 配置（在参数解析前，使 .env 中的值可作为默认值）
if [[ -f "$PROJECT_DIR/.env" ]]; then
  # shellcheck disable=SC1091
  source "$PROJECT_DIR/.env"
  echo "[INFO] 已加载 .env 配置"
fi

DEFAULT_BOARD="zectrix-s3-epaper-4.2"
DEFAULT_OTA_URL="${DEFAULT_OTA_URL:-https://ota.zectrix.com/xiaozhi/ota/}"

REBUILD=true
OTA_URL="$DEFAULT_OTA_URL"

POSITIONAL_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild)
      REBUILD=true
      shift
      ;;
    --no-rebuild)
      REBUILD=false
      shift
      ;;
    --ota-url)
      if [[ $# -lt 2 ]]; then
        echo "[ERROR] --ota-url 需要传入地址" >&2
        exit 1
      fi
      OTA_URL="$2"
      shift 2
      ;;
    -h|--help)
      cat <<'EOF'
用法:
  ./build.sh [--no-rebuild] [--ota-url URL] [board_type] [build_name]

示例:
  ./build.sh
  ./build.sh --no-rebuild
  ./build.sh --ota-url https://ota.zectrix.com/xiaozhi/ota/
  ./build.sh --no-rebuild zectrix-s3-epaper-4.2 zectrix-s3-epaper-4.2

默认值:
  board_type = zectrix-s3-epaper-4.2
  build_name = 与 board_type 相同
  ota_url = .env 中的 DEFAULT_OTA_URL 或 https://ota.zectrix.com/xiaozhi/ota/

参数:
  --no-rebuild   跳过 fullclean，增量编译（默认执行 fullclean 完整重编译）
  --ota-url URL  指定打包写入的 CONFIG_OTA_URL
EOF
      exit 0
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        POSITIONAL_ARGS+=("$1")
        shift
      done
      ;;
    -*)
      echo "[ERROR] 未知参数: $1" >&2
      exit 1
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#POSITIONAL_ARGS[@]} -gt 2 ]]; then
  echo "[ERROR] 参数过多，仅支持 [board_type] [build_name]" >&2
  exit 1
fi

BOARD_TYPE="${POSITIONAL_ARGS[0]:-$DEFAULT_BOARD}"
BUILD_NAME="${POSITIONAL_ARGS[1]:-$BOARD_TYPE}"

if [[ -z "$OTA_URL" ]]; then
  echo "[ERROR] OTA 地址不能为空" >&2
  exit 1
fi

# ESP-IDF 环境加载顺序（命中即返回）：
#   1. 当前 shell 已有 idf.py（例如已执行过 aidf / source export.sh）→ 直接复用
#   2. 显式指定的 export.sh：IDF_EXPORT_PATH > IDF_PATH
#   3. EIM 安装自动探测（~/.espressif/tools/activate_idf_*.sh，取版本号最高的）
#   4. 常见安装路径下的 export.sh：~/.espressif/v*/esp-idf、~/esp/esp-idf、/root/esp/esp-idf
#   5. 都找不到才报错
#
# 注意：release.py 用 os.system("idf.py ...") 调用，走的是 /bin/sh 子进程。
# EIM 的 activate 脚本只把 idf.py 定义成 shell 函数，子进程看不见，
# 因此每一步成功后都会把 $IDF_PATH/tools 补进 PATH（见 ensure_idf_tools_on_path）。
#
# 可用环境变量覆盖（也可写入 firmware/.env，已在 .gitignore 中）：
#   IDF_EXPORT_PATH=/Users/you/.espressif/v6.1-rc1/esp-idf
#   IDF_ACTIVATE_SCRIPT=/Users/you/.espressif/tools/activate_idf_v6.1-rc1.sh
#   IDF_TOOLS_PATH=/Users/you/.espressif/tools

# EIM 安装的 activate 脚本，取字典序最后一个（对应版本号最大）
find_idf_activate_script() {
  if [[ -n "${IDF_ACTIVATE_SCRIPT:-}" ]]; then
    printf '%s' "$IDF_ACTIVATE_SCRIPT"
    return 0
  fi

  local tools_dir="${IDF_TOOLS_PATH:-$HOME/.espressif/tools}"
  [[ -d "$tools_dir" ]] || return 0

  local prior_nullglob
  prior_nullglob="$(shopt -p nullglob 2>/dev/null || echo 'shopt -u nullglob')"
  shopt -s nullglob
  local candidates=("$tools_dir"/activate_idf_*.sh)
  eval "$prior_nullglob"

  [[ ${#candidates[@]} -gt 0 ]] || return 0
  printf '%s' "${candidates[${#candidates[@]}-1]}"
}

# EIM 的 activate 脚本把 idf.py 定义成 shell 函数（或别名），子进程看不见。
# 而 scripts/release.py 是用 os.system("idf.py ...") 调的（走 /bin/sh 子进程），
# 所以必须把 $IDF_PATH/tools 也放进 PATH，让 idf.py 以可执行文件形式可被解析。
ensure_idf_tools_on_path() {
  [[ -n "${IDF_PATH:-}" ]] || return 0
  [[ -d "$IDF_PATH/tools" ]] || return 0

  local tools_dir="$IDF_PATH/tools"
  case ":$PATH:" in
    *":$tools_dir:"*) return 0 ;;
  esac

  export PATH="$tools_dir:$PATH"
  echo "[INFO] 已将 $tools_dir 加入 PATH（供子进程调用 idf.py）"
}

# 把 KEY=VALUE 形式的文本逐行 export
apply_env_lines() {
  local line key value
  while IFS= read -r line; do
    [[ "$line" == *=* ]] || continue
    key="${line%%=*}"
    value="${line#*=}"
    # SYSTEM_PATH 是安装时快照的宿主机 PATH，不能拿来覆盖当前 PATH
    [[ "$key" == "SYSTEM_PATH" ]] && continue
    export "$key=$value"
  done
}

# EIM 的 activate 脚本不能直接 source：它靠 $0 判断“是否被 source”，
# 从被执行的脚本里 source 会被误判并在末尾 exit 1。改用官方的 `-e` 模式
# 打印 KEY=VALUE 环境变量再 export，效果等价且对子进程友好。
load_eim_env() {
  apply_env_lines < <(bash "$1" -e 2>/dev/null)
  [[ -n "${IDF_PATH:-}" ]]
}

# export.sh 在 detect_python 失败时会直接 exit，直接 source 会连带杀掉本脚本，
# 因此放在子 shell 里加载，再把需要的变量取回来。
IDF_ENV_VARS=(
  PATH IDF_PATH IDF_TOOLS_PATH IDF_PYTHON_ENV_PATH IDF_VERSION ESP_IDF_VERSION
  ESP_ROM_ELF_DIR OPENOCD_SCRIPTS IDF_COMPONENT_LOCAL_STORAGE_URL ESP_CLANG_LIBS_PATH
)

load_export_env() {
  local root="$1"
  local dump

  dump="$(bash -c '
    set +eu
    source "$1/export.sh" >/dev/null 2>&1 || exit 1
    for v in "${@:2}"; do
      printf "%s=%s\n" "$v" "${!v}"
    done
  ' _ "$root" "${IDF_ENV_VARS[@]}" 2>/dev/null)" || return 1

  apply_env_lines <<<"$dump"
  command -v idf.py >/dev/null 2>&1
}

# 依次尝试给定根目录下的 export.sh，成功返回 0
try_export_sh() {
  [[ $# -gt 0 ]] || return 1

  local root
  for root in "$@"; do
    [[ -f "$root/export.sh" ]] || continue
    echo "[INFO] 加载 ESP-IDF 环境: $root/export.sh"
    load_export_env "$root"
    ensure_idf_tools_on_path
    if command -v idf.py >/dev/null 2>&1; then
      return 0
    fi
  done
  return 1
}

load_idf_env() {
  if command -v idf.py >/dev/null 2>&1; then
    echo "[INFO] 复用当前 shell 的 idf.py: $(command -v idf.py)"
    ensure_idf_tools_on_path
    return 0
  fi

  # 显式指定优先于自动探测
  local explicit_roots=()
  if [[ -n "${IDF_EXPORT_PATH:-}" ]]; then
    explicit_roots+=("$IDF_EXPORT_PATH")
  fi
  if [[ -n "${IDF_PATH:-}" ]]; then
    explicit_roots+=("$IDF_PATH")
  fi
  if [[ ${#explicit_roots[@]} -gt 0 ]] && try_export_sh "${explicit_roots[@]}"; then
    return 0
  fi

  # EIM 安装自动探测
  local activate_script
  activate_script="$(find_idf_activate_script)"
  if [[ -n "$activate_script" && -f "$activate_script" ]]; then
    echo "[INFO] 加载 ESP-IDF 环境 (EIM): $activate_script"
    load_eim_env "$activate_script"
    ensure_idf_tools_on_path
    if command -v idf.py >/dev/null 2>&1; then
      return 0
    fi
  fi

  # 常见安装路径兜底
  if try_export_sh "$HOME"/.espressif/v*/esp-idf "$HOME"/esp/esp-idf /root/esp/esp-idf; then
    return 0
  fi

  # 兜底：IDF_PATH 已指向一个安装，但环境未初始化过
  if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/tools/idf.py" ]]; then
    echo "[WARN] IDF_PATH 已设置但未初始化环境，仅补齐 PATH" >&2
    ensure_idf_tools_on_path
    return 0
  fi

  if ! command -v idf.py >/dev/null 2>&1; then
    cat >&2 <<'EOF'
[ERROR] 未检测到 idf.py，请先安装 ESP-IDF。可任选其一：
  1) EIM 安装：https://github.com/espressif/idf-im-ui （会在 ~/.espressif/tools 生成 activate_idf_*.sh）
  2) 手动安装后执行：source <IDF目录>/export.sh
  3) 在 firmware/.env 中指定：IDF_EXPORT_PATH=/path/to/esp-idf
EOF
    exit 1
  fi
}

if ! command -v python3 >/dev/null 2>&1; then
  echo "[ERROR] 未检测到 python3，请先安装 Python 3" >&2
  exit 1
fi

cd "$PROJECT_DIR"
load_idf_env

# release.py 用 os.system("idf.py ...")，走 /bin/sh 子进程。
# 这里用同样的视角校验一次，避免“本 shell 能跑、子进程找不到”的假成功。
if ! /bin/sh -c 'command -v idf.py' >/dev/null 2>&1; then
  echo "[ERROR] idf.py 无法在子进程中解析，请检查 IDF 环境（当前 IDF_PATH=${IDF_PATH:-未设置}）" >&2
  exit 1
fi
echo "[INFO] ESP-IDF: ${IDF_VERSION:-${ESP_IDF_VERSION:-未知版本}} @ ${IDF_PATH:-未设置}"

BOARD_CONFIG_DIR="main/boards/${BOARD_TYPE}"
BOARD_CONFIG_PATH="${BOARD_CONFIG_DIR}/config.json"
TEMP_CONFIG_NAME="config.build.sh.$$.json"
TEMP_CONFIG_PATH="${BOARD_CONFIG_DIR}/${TEMP_CONFIG_NAME}"

if [[ ! -f "$BOARD_CONFIG_PATH" ]]; then
  echo "[ERROR] 未找到板型配置文件: $BOARD_CONFIG_PATH" >&2
  exit 1
fi

cleanup_temp_config() {
  if [[ -f "$TEMP_CONFIG_PATH" ]]; then
    rm -f "$TEMP_CONFIG_PATH"
  fi
}

trap cleanup_temp_config EXIT

python3 - "$BOARD_CONFIG_PATH" "$TEMP_CONFIG_PATH" "$BUILD_NAME" "$OTA_URL" \
  "${DEFAULT_WIFI_SSID:-}" "${DEFAULT_WIFI_PASSWORD:-}" <<'PY'
import json
import os
import sys

config_path, output_path, build_name, ota_url, wifi_ssid, wifi_password = sys.argv[1:7]

with open(config_path, "r", encoding="utf-8") as f:
    config = json.load(f)

build = None
for item in config.get("builds", []):
    if item.get("name") == build_name:
        build = item
        break

if build is None:
    print(f"[ERROR] 在 {config_path} 中未找到 build_name={build_name}", file=sys.stderr)
    sys.exit(1)

sdkconfig_append = build.setdefault("sdkconfig_append", [])

# 需要注入的 sdkconfig 键值对
injections = {
    "CONFIG_OTA_URL": f'"{ota_url}"',
}
if wifi_ssid:
    injections["CONFIG_DEFAULT_WIFI_SSID"] = f'"{wifi_ssid}"'
    injections["CONFIG_DEFAULT_WIFI_PASSWORD"] = f'"{wifi_password}"'

for key, value in injections.items():
    entry = f"{key}={value}"
    for i, existing in enumerate(sdkconfig_append):
        if isinstance(existing, str) and existing.startswith(f"{key}="):
            sdkconfig_append[i] = entry
            break
    else:
        sdkconfig_append.append(entry)

with open(output_path, "w", encoding="utf-8") as f:
    json.dump(config, f, ensure_ascii=False, indent=4)
    f.write("\n")
PY

if [[ "$REBUILD" == "true" ]]; then
  echo "[INFO] --rebuild 已启用，清理 build 目录并删除旧发布包"
  # 用系统 rm：WorkBuddy 沙箱的 rm shim 缺 dirname，会误报失败
  /bin/rm -rf build
  shopt -s nullglob
  old_zips=(releases/v*_${BUILD_NAME}.zip)
  if [[ ${#old_zips[@]} -gt 0 ]]; then
    rm -f "${old_zips[@]}"
  fi
  shopt -u nullglob
fi

if [[ -n "${DEFAULT_WIFI_SSID:-}" ]]; then
  echo "[INFO] 预设WiFi: $DEFAULT_WIFI_SSID"
fi

echo "[INFO] 开始打包: board_type=$BOARD_TYPE, build_name=$BUILD_NAME, ota_url=$OTA_URL"
python3 scripts/release.py "$BOARD_TYPE" --config "$TEMP_CONFIG_NAME" --name "$BUILD_NAME"

LATEST_ZIP="$(ls -1t releases/v*_${BUILD_NAME}.zip 2>/dev/null | head -n 1 || true)"

if [[ -z "$LATEST_ZIP" ]]; then
  echo "[ERROR] 打包完成但未找到 releases/v*_${BUILD_NAME}.zip" >&2
  exit 1
fi

# GNU stat 用 -c%s，BSD/macOS stat 用 -f%z；直接用 wc 更省心
file_size() {
  wc -c <"$1" | tr -d '[:space:]'
}

ZIP_SIZE="0"
BIN_PATH="build/merged-binary.bin"
BIN_SIZE="0"

if [[ -f "$LATEST_ZIP" ]]; then
  ZIP_SIZE="$(file_size "$LATEST_ZIP")"
fi

echo "[OK] 打包完成"
echo "[OK] ZIP: $LATEST_ZIP (${ZIP_SIZE} bytes)"
echo "[OK] BIN: $BIN_PATH (${BIN_SIZE} bytes)"

# ── 同步固件到管理后台，支持网页一键刷写 ──
if [[ -f "$BIN_PATH" ]]; then
  MANAGER_NEXT_DIR="$PROJECT_DIR/../main/manager-next"
  FW_VERSION="$(basename "$LATEST_ZIP" .zip | sed "s/_${BUILD_NAME}$//")"
  FW_TIME="$(date '+%Y-%m-%d %H:%M:%S')"

  for target_dir in "$MANAGER_NEXT_DIR/public/firmware" "$MANAGER_NEXT_DIR/out/firmware"; do
    mkdir -p "$target_dir"
    cp "$BIN_PATH" "$target_dir/merged-binary.bin"
    cp "$PROJECT_DIR/changelog.md" "$target_dir/changelog.md"
    cat > "$target_dir/firmware-info.json" <<FWEOF
{
  "version": "$FW_VERSION",
  "board": "$BOARD_TYPE",
  "buildName": "$BUILD_NAME",
  "buildTime": "$FW_TIME",
  "fileSize": $BIN_SIZE,
  "fileName": "merged-binary.bin"
}
FWEOF
  done

  echo "[OK] 已同步固件及更新日志到管理后台 (public + out)"

  # ── 复制 OTA 固件到 data/bin/，供设备自动更新 ──
  OTA_BIN="build/xiaozhi.bin"
  OTA_BIN_DIR="$PROJECT_DIR/../main/xiaozhi-server/data/bin"
  if [[ -f "$OTA_BIN" ]]; then
    mkdir -p "$OTA_BIN_DIR"
    OTA_BIN_NAME="${BUILD_NAME}_${FW_VERSION#v}.bin"
    cp "$OTA_BIN" "$OTA_BIN_DIR/$OTA_BIN_NAME"
    echo "[OK] 已复制 OTA 固件: data/bin/$OTA_BIN_NAME ($(file_size "$OTA_BIN") bytes)"
  else
    echo "[WARN] 未找到 $OTA_BIN，跳过 OTA 固件复制"
  fi
fi
