// Copyright 2026 Phinomenon Inc.
//
// Phi Browser companion extension for Pi. A skill subprocess cannot wake an
// idle Pi session; this extension runs inside Pi and owns the supported
// in-process delivery API.

import { homedir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

type Bridge = { start(): void; stop(): void };

// Pi's extension loader resolves relative imports from the installed symlink
// path (~/.pi/agent/extensions/phi-browser), not from the link's source inside
// the bundled skill. Load the shared bridge through the skill's canonical Pi
// install path so app-bundle updates and source-checkout links both work.
const bridgeModuleURL = pathToFileURL(join(
  homedir(), ".pi", "agent", "skills", "phi-browser",
  "scripts", "lib", "mirror-pi-bridge.mjs",
)).href;

export default function (pi: ExtensionAPI) {
  let bridge: Bridge | null = null;
  let generation = 0;

  pi.on("session_start", async (_event, ctx) => {
    const currentGeneration = ++generation;
    bridge?.stop();
    bridge = null;

    const { PiConsoleBridge } = await import(bridgeModuleURL);
    // A fast /reload or session switch may have shut this extension instance
    // down while the dynamic import was resolving. Never revive the old one.
    if (currentGeneration !== generation) return;

    bridge = new PiConsoleBridge({
      sessionKey: ctx.sessionManager.getSessionId(),
      agentPid: process.pid,
      sendUserMessage: (text: string, options: { deliverAs: "steer" }) => {
        pi.sendUserMessage(text, options);
      },
    });
    bridge.start();
  });

  pi.on("session_shutdown", () => {
    generation += 1;
    bridge?.stop();
    bridge = null;
  });
}
