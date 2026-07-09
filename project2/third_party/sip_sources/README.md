# SIP 依赖源码包

将以下文件放入此目录，`scripts/install_sip_local.sh` 会优先使用本地文件，无需联网：

- `libosip2-5.3.0.tar.gz`
- `libeXosip2-5.3.0.tar.gz`

**获取方式：**

1. 自动下载：`bash scripts/download_sip_sources.sh`
2. 手动下载（国内可用 USTC 镜像）：
   - libosip2: http://mirrors.ustc.edu.cn/debian/pool/main/libo/libosip2/libosip2_5.3.0.orig.tar.gz
   - libeXosip2: https://github.com/siptools/libeXosip2/releases/download/5.3.0/libeXosip2-5.3.0.tar.gz

若将本目录连同两个 tarball 一并提交到仓库，他人克隆后可直接运行 `install_sip_local.sh`，无需网络。

**他人拿到项目后的完整步骤：**

```bash
# 1. 安装 SIP 库（若 third_party/sip_sources 已有 tarball 则无需联网）
bash scripts/install_sip_local.sh

# 2. 编译
cd build && rm -rf CMakeCache.txt CMakeFiles && cmake .. && make -j$(nproc)

# 3. 若拷贝项目到其他路径，需修复库的 RUNPATH
bash scripts/fix_sip_rpath.sh

# 4. 运行
./build/demo
```
