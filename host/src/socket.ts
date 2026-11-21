// Where the host listens for local clients (the Claude Code hook, `macdeck notify`).
// A Unix socket in a private directory: only this user can connect.
import os from "node:os";
import path from "node:path";

export const SOCKET_DIR = path.join(os.homedir(), ".macdeck");
export const SOCKET = path.join(SOCKET_DIR, "host.sock");
