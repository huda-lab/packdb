# GitHub Actions Release Workflow

This document describes the `PackDBRelease.yml` GitHub Actions workflow that builds release binaries for PackDB across Windows, Linux, and macOS platforms.

## Overview

The release workflow is a **manually triggered** workflow (`workflow_dispatch`) that:
1. Builds platform-specific binaries for Linux, macOS, and Windows
2. Packages CLI executables and development libraries
3. Creates a draft GitHub release with all artifacts

## Workflow Inputs

When triggering the workflow manually, you can configure:

| Input | Description | Default |
|-------|-------------|---------|
| `version` | Release version tag (e.g., `v0.1.0-beta`) | Required |
| `platforms` | Comma-separated platforms: `linux`, `macos`, `windows` | `linux` |
| `create_release` | Whether to create a GitHub release | `true` |

## Job Structure

The workflow consists of **4 jobs** that can run in parallel (platform builds) plus a final release job:

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  linux-x64      │  │  macos-universal│  │  windows-x64    │
│  (ubuntu-latest)│  │  (macos-14)     │  │  (windows-2019) │
└────────┬────────┘  └────────┬────────┘  └────────┬────────┘
         │                    │                    │
         └────────────────────┼────────────────────┘
                              ▼
                   ┌─────────────────────┐
                   │   create-release    │
                   │   (ubuntu-latest)   │
                   └─────────────────────┘
```

---

## Job 1: Linux x86-64 Build

**Runner:** `ubuntu-latest`  
**Condition:** Runs if `platforms` input contains `linux`

### Build Process

1. **Checkout code** with full git history (`fetch-depth: 0`)
2. **Setup Python 3.12** for build scripts
3. **Build using Docker** with `manylinux2014_x86_64` image:
   - This ensures compatibility with older Linux distributions (glibc 2.17+)
   - Installs `perl-IPC-Cmd` dependency for OpenSSL build
   - Runs `make` to compile the project

### Build Verification

```bash
./build/release/packdb -c "PRAGMA platform;"
./build/release/packdb --version
```

### Packaged Artifacts

| Artifact | Contents |
|----------|----------|
| `packdb_cli-linux-amd64.zip` | CLI executable (zip) |
| `packdb_cli-linux-amd64.gz` | CLI executable (gzip) |
| `libpackdb-linux-amd64.zip` | Shared library (`.so`), static library (`.a`), headers |

---

## Job 2: macOS Universal Build

**Runner:** `macos-14` (Apple Silicon runner)  
**Condition:** Runs if `platforms` input contains `macos`

### Build Process

1. **Checkout code** with full git history
2. **Setup Python 3.12** and **Ninja** build system
3. **Setup Ccache** for build caching (speeds up repeated builds)
4. **Build universal binary** with `OSX_BUILD_UNIVERSAL=1`:
   - Creates a single binary supporting both Intel (x86_64) and Apple Silicon (arm64)

### Build Verification

```bash
./build/release/packdb -c "PRAGMA platform;"
./build/release/packdb --version
file build/release/packdb  # Verifies universal binary
```

### Packaged Artifacts

| Artifact | Contents |
|----------|----------|
| `packdb_cli-osx-universal.zip` | CLI executable (zip) |
| `packdb_cli-osx-universal.gz` | CLI executable (gzip) |
| `libpackdb-osx-universal.zip` | Dynamic library (`.dylib`), headers |

---

## Job 3: Windows x64 Build

**Runner:** `windows-2019`  
**Condition:** Runs if `platforms` input contains `windows`

### Build Process

1. **Checkout code** with full git history
2. **Setup Python 3.12** and **Ccache**
3. **Build with CMake**:
   ```bash
   cmake -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_GENERATOR_PLATFORM=x64 \
         -DDISABLE_UNITY=1 \
         -DOVERRIDE_GIT_DESCRIBE="${version}"
   cmake --build . --config Release --parallel
   ```

### Build Verification

```bash
Release/packdb.exe -c "PRAGMA platform;"
Release/packdb.exe --version
```

### Packaged Artifacts

| Artifact | Contents |
|----------|----------|
| `packdb_cli-windows-amd64.zip` | CLI executable (`.exe`) |
| `libpackdb-windows-amd64.zip` | DLL (`.dll`), import library (`.lib`), headers |

---

## Job 4: Create GitHub Release

**Runner:** `ubuntu-latest`  
**Condition:** Runs after all platform builds complete (uses `needs` and `always()`)  
**Dependencies:** `linux-x64`, `macos-universal`, `windows-x64`

### Process

1. **Download all artifacts** from previous jobs
2. **Generate release notes** with:
   - Download links for each platform
   - Quick start instructions
   - System requirements
3. **Create draft release** using GitHub CLI:
   ```bash
   gh release create ${version} \
     --title "PackDB ${version}" \
     --notes-file release_notes.md \
     --draft \
     artifacts/**/*.zip artifacts/**/*.gz
   ```

The release is created as a **draft** so maintainers can review before publishing.

---

## Key Environment Variables

| Variable | Purpose |
|----------|---------|
| `OVERRIDE_GIT_DESCRIBE` | Sets the version string embedded in binaries |
| `GH_TOKEN` | GitHub token for creating releases |
| `OSX_BUILD_UNIVERSAL` | Enables universal binary build on macOS |
| `CMAKE_BUILD_PARALLEL_LEVEL` | Number of parallel build jobs |

---

## Artifact Retention

All uploaded artifacts are retained for **7 days** (`retention-days: 7`).

---

## How to Trigger the Workflow

1. Go to the repository's **Actions** tab
2. Select **PackDB Release Build** from the workflow list
3. Click **Run workflow**
4. Fill in the inputs:
   - **version**: e.g., `v0.1.0-beta`
   - **platforms**: e.g., `linux,macos,windows`
   - **create_release**: Check to create a draft release
5. Click **Run workflow**

---

## System Requirements for Built Binaries

| Platform | Requirements |
|----------|--------------|
| **Linux** | CentOS 7+, Ubuntu 18.04+, Debian 10+ (glibc 2.17+) |
| **macOS** | macOS 11.0 (Big Sur) or later |
| **Windows** | Windows 10+, Visual C++ Redistributable 2019+ |

---

## Notes

- **Unsigned binaries**: The release binaries are not code-signed (unlike upstream DuckDB which uses Azure Trusted Signing for Windows and Apple Developer signing for macOS)
- **manylinux2014**: The Linux build uses manylinux2014 for maximum compatibility
- **Universal binary**: The macOS build produces a single binary that runs natively on both Intel and Apple Silicon Macs
- **Ccache**: macOS and Windows builds use ccache to speed up subsequent builds
