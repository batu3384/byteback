# Wolf Recovery

![Wolf Recovery Logo](https://via.placeholder.com/150x150?text=Wolf+Logo)

A high-performance physical data recovery tool utilizing a native Windows C++ engine combined with a modern Electron/React interface. Built to bypass Windows APIs to read raw disk sectors, recovering lost files based on NTFS file system structures and file signatures.

## Features
- Raw physical disk access
- MFT (Master File Table) parsing for NTFS
- Signature-based file recovery (carving)
- Modern React-based Dashboard
- High-performance SQLite backing store
- Electron desktop integration

## Tech Stack
| Component | Technology |
| --- | --- |
| Frontend | React, TypeScript, Tailwind CSS, Vite |
| Desktop App | Electron, IPC Main/Renderer |
| Backend Engine | C++, Windows APIs (DeviceIoControl) |
| Native Bridge | Node-API (N-API) |
| Database | SQLite |

## Prerequisites
To build and run Wolf Recovery, you need the following installed:
- Visual Studio 2022 Build Tools (with "Desktop development with C++" workload)
- Node.js 20+
- CMake 3.20+

## Build Instructions
```bash
# Clone the repository
git clone <repo-url>
cd wolf-recovery

# Install Node dependencies
npm install

# Build the native engine
npm run build:native

# Start the application in development mode
npm run dev
```

## Project Structure
```
wolf-recovery/
├── src/
│   ├── main/           # Electron main process (IPC, Native bindings)
│   ├── preload/        # ContextBridge for secure IPC
│   ├── renderer/       # React frontend application
│   │   ├── components/ # Reusable UI components
│   │   ├── pages/      # Page views (Dashboard)
│   │   └── App.tsx     # Main React app entry
├── native/
│   ├── engine/         # C++ disk scanning engine
│   ├── io/             # C++ SQLite output writer
│   ├── bridge/         # N-API native add-on bindings
│   └── CMakeLists.txt  # CMake configuration
└── package.json        # Project metadata and build scripts
```

## Development Roadmap
- **Phase 1**: Foundation (Project scaffold, native engine, I/O layer, SQLite store, IPC, Dashboard UI)
- **Phase 2**: Deep Scan Engine (MFT parsing, Raw I/O, Node-API streaming)
- **Phase 3**: File Carving (Signature detection, parallel processing, recovery module)
- **Phase 4**: Recovery UI & Progress (Live scan view, file preview, detailed progress bars)
- **Phase 5**: Final Polish (Packaging, error handling, performance tuning, release build)

## License
MIT License
