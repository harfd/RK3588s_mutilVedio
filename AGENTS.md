# AGENTS.md — GB/T 28181 Video Push-Streaming for Rockchip

## Active code
- `project2/` is the current working copy. The `项目/项目/项目/` tree contains older copies — do not edit.

## Platform
- **Target**: ARM Linux, Rockchip platform with MPP hardware encoder.
- **NOT** for Windows/x86 — uses Rockchip MPP kernel driver and `/dev/vcodec_service`.

## Quick start (full order)
```bash
cd project2
bash scripts/install_sip_local.sh      # builds osip2 + eXosip2 into install/
cd build && rm -rf CMakeCache.txt CMakeFiles
cmake .. && make -j$(nproc)
bash ../scripts/fix_sip_rpath.sh         # only needed if project dir moved
bash ../scripts/run_demo.sh              # sets LD_LIBRARY_PATH then runs
```

## Build
- **System**: CMake 3.4+, C++17, `-Wall -g -pthread`
- **Output**: `build/demo`
- **Deps**: OpenCV (system apt or `MPP/opencv4-1/`), eXosip2/osip2 (`install/lib/`), Rockchip MPP SDK (`MPP/lib/`)
- The project is **self-contained**: all third-party libs install under `install/` and `MPP/`.
- If moving the project directory, run `scripts/fix_sip_rpath.sh` to fix RUNPATH in SIP `.so` files.
- CMake auto-detects system OpenCV first; falls back to bundled `MPP/opencv4-1/`. If neither exists, build fails.

## Run
- `bash scripts/run_demo.sh` or `./build/demo`
- Config: `gb28181.conf` in CWD (key=value, lines starting with `#` are comments). If absent, built-in defaults apply (see `config.cc:80-90`).
- **Must edit** `main.cc:44` to point to a real MP4 file path (hardcoded to `/home/cat/project2/test.mp4`).
- `pushPort` (default 7000) must match the port the GB28181 platform expects for media push.

## Architecture
```
main.cc → pushToGB28181() → SipThread (SIP event loop, eXosip2)
        → decode_from_mp4() → set_Mat(frame) → matData queue
                                                 ↓
                                          encode() thread: MPP HW encoder → H264 → h264data queue
                                                                                      ↓
                                                                               work() thread: PS/RTP/UDP push
```
- Threads: SIP, encode, work (push), decode — all managed in `push.cc`.
- Frame queues: `matData(10)` (cv::Mat), `h264data(50)` (H264Frame). Blocking with back-pressure.
- If `matData` fills (10 frames), `checkQueueThread()` clears it to unblock producer.

## Video pipeline
| Stage | File | Key detail |
|-------|------|------------|
| Input | `main.cc` | OpenCV `VideoCapture` + 10ms sleep |
| Encode | `decode.cc` | MPP HW encode → H264 Baseline, CABAC, 4Mbps, 30fps, GOP=30, level 3.1, 1280×720 |
| Packetize | `ps_rtp.cc` | MPEG-PS + RTP, bit-level header writing, 1400-byte payload max, PT=96 |
| Push | `push.cc` | UDP, `poll()` + 3 retries |
| Signaling | `gb28181.cc` | SIP REGISTER (MD5), INVITE, MESSAGE (Catalog/DeviceInfo/DeviceControl), SUBSCRIBE (Catalog/MobilePosition). Re-registers every 30s. |

## Code conventions
- Chinese comments throughout — keep them, do not rewrite in English.
- `FrameQueue<T>` template in `frame_queue.h` — bounded, thread-safe with `condition_variable`.
- Config struct in `config.h` — singleton pattern in `config.cc`.
- `MppEncoder` is a singleton in `decode.h`.

## Testing / CI
- **No test suite, no CI, no linter, no typecheck.** The only verification is `make` succeeding and `./build/demo` running.
- Test MP4 provided as `test.mp4` in `project2/` (also a copy in `project2/MPP/`).

## License
Non-commercial only. Copyright HeXiaotian 2025. Do not redistribute, resell, or create derivative works without written permission.

## Common pitfalls
- `main.cc:44` has a hardcoded absolute path — change it per deployment.
- Build must happen inside `build/` after removing `CMakeCache.txt` and `CMakeFiles/` when reconfiguring.
- `gb28181.conf` is loaded from the **current working directory**, not from `project2/`.
- The project uses local `install/lib` — do not `sudo make install` system-wide.
- If OpenCV VideoCapture fails, the decode thread silently exits — check `test.mp4` path.
- `output.ps` in `build/` is a run artifact (PS stream dump), not source code.
