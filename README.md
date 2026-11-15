<div align="center">

# ⚡ go-brrr

### Tiny single-threaded, readiness-driven web server with SQLite-backed user CRUD

[![Go](https://img.shields.io/badge/Go-00ADD8?style=for-the-badge&logo=go&logoColor=white)](https://go.dev)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)](LICENSE)

</div>

---

## 📖 Overview

A tiny single-threaded, readiness-driven web server with a SQLite-backed user CRUD API. Built to host as a service. Includes benchmark implementations.

## 🏗 Project structure

```
go-brrr/
├── implementations/   # server implementations
├── bench/             # benchmarks
├── Makefile
└── Dockerfile
```

## 🛠 Tech stack

`Go` · `SQLite` · `Docker`

## 📦 Quick start

```bash
git clone https://github.com/m3rashid/go-brrr.git
cd go-brrr
make build
./bin/server
```

## 📄 License

MIT © [m3rashid](https://github.com/m3rashid)
