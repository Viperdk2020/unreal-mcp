# Unreal MCP — Remote Control Interface for Unreal Engine 5.6

Unreal MCP enables external AI assistants and automation systems to control Unreal Engine through a clean, structured Model Context Protocol (MCP).  
This plugin provides a safe, extensible, and editor‑friendly bridge between Unreal Engine 5.6 and remote tools such as Python servers, assistants (e.g., Claude, Windsurf, Cursor), and automated pipelines.

This README is intentionally streamlined for clarity. It focuses on concepts, usage, installation, and major features rather than internal implementation details.

---

## ✨ Overview

Unreal MCP exposes a set of editor‑level tools over a local TCP connection using a stable, framed JSON protocol.  
These tools allow external clients to:

- Inspect and manipulate project assets  
- Spawn, modify, and organize actors  
- Work with Sequencer  
- Manage materials and Niagara systems  
- Run builds and automated tests  
- Navigate the editor (camera, viewport, selection)  
- Perform batch content management operations  

Safety-first design ensures the editor cannot be modified unless explicitly allowed.

---

## 🚀 Key Features

### 1. Structured, Reliable MCP Protocol
- Binary frame boundary (length + JSON)
- Versioned handshake
- Heartbeats and connection timeouts
- Session resume and safe deduplication
- Clear error schema

### 2. Strict Security Model
- Read-only by default  
- Allow/Deny tool lists  
- Allowed/Forbidden content roots  
- Optional source‑control enforcement  
- Audit reporting and request validation  
- Granular rate-limiting & payload size limits  

### 3. Asset Tools
Allows external clients to:
- Find assets
- Inspect metadata
- Create folders
- Rename or delete assets
- Fix redirectors
- Run batch import with presets

### 4. Level & Actor Tools
- Load and save maps
- Spawn, destroy, attach, or transform actors
- Modify tags
- Select actors and control viewport focusing

### 5. Material Tools
- Create Material Instances
- Set scalar, vector, texture, and switch parameters
- Apply materials to components or entire actors

### 6. Sequencer Tools
Simplified creation and manipulation of Level Sequences:
- Create sequences
- Bind actors
- Add tracks and camera cuts
- Export sequences for external processing

### 7. Niagara Tools
- Spawn Niagara components  
- Set User Parameters  
- Activate / Deactivate systems  

### 8. Build & Automation Tools
- Run UAT BuildCookRun  
- Execute Automation Tests  
- Trigger Gauntlet testing  

---

## 🧩 Installation

### 1. Enable the Plugin
Clone this project (or your fork) and open the included Unreal project:

```
MCPGameProject.uproject
```

Enable the **Unreal MCP** plugin if it's not already active.

### 2. Start the MCP Server

A Python server is included under:

```
Python/
```

Start it:

```bash
python unreal_mcp_server.py
```

### 3. Configure the Plugin

In Unreal Editor:

**Project Settings → Plugins → Unreal MCP**

Recommended defaults:

- Host: 127.0.0.1
- Port: 12029
- Auto-connect on startup: optional

### 4. Diagnostics
- Test Connection  
- Send Ping  
- Open Logs Folder  

---

## 🛡 Security Summary

Unreal MCP is safe to run by default:

| Setting | Default | Meaning |
|--------|---------|---------|
| AllowWrite | false | Prevents any editor modifications |
| DryRun | true | Mutations return a plan instead of executing |
| RequireCheckout | false | SCM not enforced |
| AllowedContentRoots | empty | Nothing is writable |

To enable write access, you must:

1. Enable AllowWrite  
2. Specify at least one /Game/... path in AllowedContentRoots  

---

## 🧭 Tool Categories

Below is a simplified categorization of the major tool families.

### Asset Tools
- asset.find
- asset.exists
- asset.metadata
- asset.create_folder
- asset.rename
- asset.delete
- asset.fix_redirectors
- asset.batch_import

### Level Tools
- level.load
- level.save_open
- level.stream_sublevel

### Actor Tools
- actor.spawn
- actor.destroy
- actor.transform
- actor.attach
- actor.tag

### Sequencer Tools
- sequence.create
- sequence.bind_actors
- sequence.add_tracks
- sequence.export

### Materials & Mesh Tools
- mi.create
- mi.set_params
- mi.batch_apply
- mesh.remap_material_slots

### Niagara Tools
- niagara.spawn_component
- niagara.set_user_params
- niagara.activate
- niagara.deactivate

### Editor Navigation Tools
- level.select
- viewport.focus
- camera.bookmark

### Build & Test Tools
- uat.buildcookrun
- automation.run_specs
- gauntlet.run

---

## 🛠 Building the Plugin (Windows Example)

```powershell
pwsh -f scripts/Build-Plugin.ps1 `
  -EngineRoot "D:\\UE_5.6" `
  -PluginUplugin ".\\MCPGameProject\\Plugins\\UnrealMCP\\UnrealMCP.uplugin" `
  -OutDir ".\\_package\\UnrealMCP_Win64" `
  -TargetPlatforms "Win64" `
  -Rocket `
  -Clean `
  -VerboseLog
```

The packaged build will appear under:

```
_package/UnrealMCP_Win64/UnrealMCP/
```

---

## 🔧 Troubleshooting

### Cannot connect (127.0.0.1:12029)
- Python server not running  
- Wrong host/port in settings  
- Firewall blocking communication  

### WRITE_NOT_ALLOWED
- Enable AllowWrite  
- Add valid paths to AllowedContentRoots  

### SOURCE_CONTROL_REQUIRED
- SCM provider not active  
- Disable RequireCheckout if not using SCM  

### High latency / timeouts
- Check antivirus / firewall  
- Adjust heartbeat or timeout settings  

---

## 📜 License & Contribution

Contributions are welcome.  
If submitting a feature or fix, please include:

- Clear use case  
- Minimal reproducible example  
- Expected vs actual behavior  

---

## ✅ Summary

Unreal MCP provides a flexible, safe, and automation-friendly way to control Unreal Engine 5.6.  
It is suitable for AI-powered workflows, large-scale automation, and editor-assisted tooling systems.

This streamlined version of the README focuses on clarity and usability while keeping all essential functionality visible.
