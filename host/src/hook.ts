// Claude Code PermissionRequest hook: hands the request to the MacDeck host so it can be answered
// on the deck, then prints Claude's decision. If the host is not running (or the request is
// answered on the Mac instead) it prints nothing, and Claude asks as usual.
//
// Installed by `node src/install-hook.ts` into ~/.claude/settings.json; input/output contract:
// https://code.claude.com/docs/en/hooks (PermissionRequest).
import net from "node:net";
import { SOCKET } from "./socket.ts";

let input = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (d) => { input += d; });
process.stdin.on("end", () => {
  let event: unknown;
  try { event = JSON.parse(input); } catch { process.exit(0); }
  const sock = net.connect(process.env.MACDECK_SOCKET ?? SOCKET);
  let reply = "";
  sock.on("connect", () => sock.write(JSON.stringify({ agent: "Claude", event }) + "\n"));
  sock.setEncoding("utf8");
  sock.on("data", (d) => { reply += d; });
  sock.on("end", () => {
    const line = reply.trim();
    if (line && line !== "{}") process.stdout.write(line + "\n");
    process.exit(0);
  });
  sock.on("error", () => process.exit(0));  // no host: Claude asks as usual
});
