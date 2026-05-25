# Janus REST Client Demo

This sample demonstrates signaling against a Janus Gateway instance using the
`janus.plugin.videocall` plugin. It now supports:

- Audio and **video** publishing via a locally captured track (falls back to a
  synthetic generator if no camera is available).
- Bidirectional **text chat** using a WebRTC data channel.
- Simple console commands to initiate and control calls.

## Prerequisites

1. Build WebRTC with tools enabled (requires a previously generated GN
   configuration, for example `gn gen out/Default --args='rtc_enable_protobuf=false'`).
2. Run a Janus Gateway instance with the videcall plugin enabled and reachable
   from this client (default REST endpoint `http://localhost:8088/janus`).
3. Ensure the Janus instance allows data channels and video for the selected
   users.

## Running the client

From the repository root after building WebRTC:

```bash
autoninja -C out/Default janus_client
out/Default/janus_client --janus_host=<host> --janus_port=<port> \
    --username=<local_user> --callee=<remote_user_if_autocall>
```

When the client reports `Registered as <user>, ready to communicate via Janus.`,
you can control it from the terminal:

- `call <peer>` – start a call with the Janus-registered peer.
- `hangup` – disconnect the active session.
- `msg <text>` – send a chat message over the WebRTC data channel.
- `quit` – terminate the client.

Incoming chat messages are printed in the console with a `[chat]` prefix. Remote
video frames are announced via periodic log entries, indicating resolution and
timestamps.

> **Tip:** If no camera is detected, the client generates synthetic video frames
> so the remote peer still receives video.