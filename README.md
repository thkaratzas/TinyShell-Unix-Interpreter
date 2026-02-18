# TinyShell (tsh) - A Unix Command Line Interpreter

A lightweight, robust Unix shell implemented in C++ that supports process management, job control, and I/O redirection. This project explores low-level system calls and the complexity of managing concurrent processes.

## ✨ Features

- **Process Management:** Forking and executing binary executables using `execvp`.
- **Job Control:** Full support for foreground and background processes (`&`), including `jobs`, `fg`, and `bg` built-in commands.
- **I/O Redirection:** - Input redirection (`<`)
  - Output redirection (`>`, `>>`)
  - Standard error redirection (`2>`)
- **Pipelining:** Support for multi-stage pipelines (e.g., `ls | grep .cpp | wc -l`).
- **Signal Handling:** Proper handling of `SIGCHLD`, `SIGINT` (Ctrl-C), and `SIGTSTP` (Ctrl-Z) to maintain shell stability.
- **Built-in Commands:** `cd`, `pwd`, `exit`, `find`, and job control primitives.

## 🛠 Technical Depth

The implementation focuses on:
- **Process Groups (PGID):** Managing terminal control and signal delivery to specific process groups.
- **Race Condition Prevention:** Using atomic flags and `sigprocmask` (concepts) for safe signal handling.
- **Dynamic Tokenization:** Custom parser to handle operators and whitespace effectively.

## 🚀 Getting Started

### Prerequisites
- GCC/G++ compiler
- Linux/Unix environment

### Compilation
Simply use the provided Makefile:
```bash
make