# Agants

- The UnrealMCP plugin now exposes an in-editor MCP TCP listener (default `127.0.0.1:55558`) alongside the legacy JSON socket (`127.0.0.1:55557`). Toggle it via Editor Settings > Plugins > Unreal MCP > Enable MCP Listener.
- MCP framing is newline-delimited JSON. Supported message types: `ping` -> `pong`, `status`, `tools` (lists tool names), `call_tool` with `tool` and optional `params`. Heartbeats (`{"type":"heartbeat"}`) may arrive if enabled; clients should ignore or log them without failing.
- Tool names match the switches in `UnrealMCPBridge::ExecuteCommand` (actors, blueprints, blueprint graph, project input, UMG). See `MCPProtocolServerRunnable` for the exported list.
- If you prefer Python FastMCP instead, you can still run `Python/unreal_mcp_server.py` and point your MCP client at it; it will connect to the plugin over the legacy port.
- From WSL, connect to the Windows host IP (the default gateway, e.g. `172.24.240.1`), not 127.0.0.1, for ports `55557/55558`.
