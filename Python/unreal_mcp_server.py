"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import socket
import sys
import json
import os
import signal
from contextlib import asynccontextmanager
import threading
import time
from typing import AsyncIterator, Dict, Any, Optional
from fastmcp import FastMCP

# Configure logging with more detailed format
logging.basicConfig(
    level=logging.DEBUG,  # Change to DEBUG level for more details
    format='%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s',
    handlers=[
        logging.FileHandler('unreal_mcp.log'),
        # logging.StreamHandler(sys.stdout) # Remove this handler to unexpected non-whitespace characters in JSON
    ]
)
logger = logging.getLogger("UnrealMCP")

# Configuration
UNREAL_HOST = os.environ.get("UNREAL_HOST", "127.0.0.1")
UNREAL_PORT = int(os.environ.get("UNREAL_PORT", "55557"))
# TCP transport for MCP (separate from the Unreal connection port)
MCP_TCP_HOST = os.environ.get("MCP_TCP_HOST", "127.0.0.1")
MCP_TCP_PORT = int(os.environ.get("MCP_TCP_PORT", "55558"))

class UnrealConnection:
    """Connection to an Unreal Engine instance."""
    
    def __init__(self):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
        self.lock = threading.Lock()
        self.heartbeat_thread: Optional[threading.Thread] = None
        self.heartbeat_stop = threading.Event()
        self.heartbeat_interval = float(os.environ.get("UNREAL_HEARTBEAT_INTERVAL", "25"))
        self.last_heartbeat: Optional[float] = None
        self.last_command: Optional[str] = None
        self.last_command_time: Optional[float] = None
        self.last_command_result: Optional[Any] = None
        self.last_command_error: Optional[str] = None
    
    def connect(self) -> bool:
        """Connect to the Unreal Engine instance."""
        with self.lock:
            try:
                if self.connected and self.socket:
                    return True
                
                logger.info(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT}...")
                self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.socket.settimeout(5)  # 5 second timeout
                
                # Set socket options for better stability
                self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                
                # Set larger buffer sizes
                self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
                self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
                
                self.socket.connect((UNREAL_HOST, UNREAL_PORT))
                self.connected = True
                self._start_heartbeat()
                logger.info("Connected to Unreal Engine")
                return True
                
            except Exception as e:
                logger.error(f"Failed to connect to Unreal: {e}")
                self.connected = False
                if self.socket:
                    try:
                        self.socket.close()
                    except Exception:
                        pass
                self.socket = None
                return False
    
    def disconnect(self):
        """Disconnect from the Unreal Engine instance."""
        with self.lock:
            self._stop_heartbeat()
            if self.socket:
                try:
                    self.socket.close()
                except Exception:
                    pass
            self.socket = None
            self.connected = False

    def receive_full_response(self, sock, buffer_size=4096) -> Dict[str, Any]:
        """Receive newline-delimited JSON responses, skipping heartbeats if present."""
        buffer = ""
        sock.settimeout(10)  # Give a bit more time in case of heartbeats
        try:
            while True:
                chunk = sock.recv(buffer_size)
                if not chunk:
                    break

                buffer += chunk.decode('utf-8')

                # Process any complete newline-delimited messages
                while '\n' in buffer:
                    raw_message, buffer = buffer.split('\n', 1)
                    raw_message = raw_message.strip()
                    if not raw_message:
                        continue

                    try:
                        message = json.loads(raw_message)
                    except json.JSONDecodeError:
                        # Put the data back and wait for more bytes
                        buffer = raw_message + ("\n" + buffer if buffer else "")
                        logger.debug("Received partial JSON message, waiting for more data...")
                        break
                    
                    # Skip protocol-level messages like heartbeats
                    if isinstance(message, dict) and message.get("type") == "heartbeat":
                        logger.debug("Received heartbeat from Unreal MCP plugin")
                        continue

                    logger.info(f"Received complete response from Unreal ({len(raw_message)} bytes)")
                    return message

            # If connection closed but we still have buffered data, try to parse it
            if buffer.strip():
                try:
                    message = json.loads(buffer)
                    logger.info(f"Received response from Unreal after socket closed ({len(buffer)} bytes)")
                    return message
                except Exception:
                    pass

            raise Exception("Connection closed before receiving a valid response")
        except socket.timeout:
            logger.warning("Socket timeout during receive")
            if buffer.strip():
                try:
                    message = json.loads(buffer)
                    logger.info(f"Using partial response after timeout ({len(buffer)} bytes)")
                    return message
                except Exception:
                    pass
            raise Exception("Timeout receiving Unreal response")
        except Exception as e:
            logger.error(f"Error during receive: {str(e)}")
            raise
    
    def send_command(self, command: str, params: Dict[str, Any] = None, track: bool = True) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response."""
        with self.lock:
            if not self.connected:
                if not self.connect():
                    logger.error("Failed to connect to Unreal Engine for command")
                    return None
            
            try:
                # MCPGameProject version expects "command" field
                command_obj = {
                    "command": command,  # MCPGameProject plugin expects "command" field
                    "params": params or {}
                }
                
                # Send with newline terminator as required by Unreal
                command_json = json.dumps(command_obj) + '\n'
                logger.info(f"Sending command: {command_json.strip()}")
                self.socket.sendall(command_json.encode('utf-8'))
                
                # Read response using improved handler
                response = self.receive_full_response(self.socket)
                
                # Track last command state
                if track:
                    self.last_command = command
                    self.last_command_time = time.time()
                    self.last_command_result = response
                    self.last_command_error = None
                
                # Log complete response for debugging
                logger.info(f"Complete response from Unreal: {response}")
                
                # Check for both error formats: {"status": "error", ...} and {"success": false, ...}
                if response.get("status") == "error":
                    error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                    logger.error(f"Unreal error (status=error): {error_message}")
                    # We want to preserve the original error structure but ensure error is accessible
                    if "error" not in response:
                        response["error"] = error_message
                    if track:
                        self.last_command_error = error_message
                elif response.get("success") is False:
                    # This format uses {"success": false, "error": "message"} or {"success": false, "message": "message"}
                    error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                    logger.error(f"Unreal error (success=false): {error_message}")
                    if track:
                        self.last_command_error = error_message
                    # Convert to the standard format expected by higher layers
                    response = {
                        "status": "error",
                        "error": error_message
                    }
                
                return response
                
            except Exception as e:
                logger.error(f"Error sending command: {e}")
                if track:
                    self.last_command = command
                    self.last_command_time = time.time()
                    self.last_command_result = None
                    self.last_command_error = str(e)
                # Always reset connection state on any error
                self.connected = False
                try:
                    self.socket.close()
                except Exception:
                    pass
                self.socket = None
                return {
                    "status": "error",
                    "error": str(e)
                }

    def _start_heartbeat(self):
        """Start background heartbeat sender to keep the connection alive."""
        if self.heartbeat_interval <= 0:
            return
        if self.heartbeat_thread and self.heartbeat_thread.is_alive():
            return

        self.heartbeat_stop.clear()

        def _loop():
            while not self.heartbeat_stop.wait(self.heartbeat_interval):
                try:
                    result = self.send_command("ping", {}, track=False)
                    self.last_heartbeat = time.time()
                    logger.debug(f"Heartbeat ping result: {result}")
                except Exception as hb_err:
                    logger.warning(f"Heartbeat failed: {hb_err}")
                    self.last_heartbeat = None
        self.heartbeat_thread = threading.Thread(target=_loop, name="UnrealMCPHeartbeat", daemon=True)
        self.heartbeat_thread.start()

    def _stop_heartbeat(self):
        """Stop the heartbeat thread."""
        if self.heartbeat_thread and self.heartbeat_thread.is_alive():
            self.heartbeat_stop.set()
            self.heartbeat_thread.join(timeout=2)
        self.heartbeat_thread = None

    def status(self) -> Dict[str, Any]:
        """Return current connection and heartbeat status."""
        return {
            "connected": self.connected,
            "heartbeat_interval": self.heartbeat_interval,
            "last_heartbeat": self.last_heartbeat,
            "heartbeat_running": bool(self.heartbeat_thread and self.heartbeat_thread.is_alive()),
            "last_command": self.last_command,
            "last_command_time": self.last_command_time,
            "last_command_error": self.last_command_error,
            "last_command_result": self.last_command_result,
        }

# Global connection state
_unreal_connection: UnrealConnection = None

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
            if not _unreal_connection.connect():
                logger.warning("Could not connect to Unreal Engine")
                _unreal_connection = None
            else:
                # Connection established; logging covers this event
                pass
        
        return _unreal_connection
    except Exception as e:
        logger.exception(f"Error getting Unreal connection: {e}")
        return None

def shutdown_server(exit_code: int = 0):
    """Gracefully shut down the MCP server process."""
    global _unreal_connection
    logger.info("Shutdown requested for Unreal MCP server")
    if _unreal_connection:
        try:
            _unreal_connection.disconnect()
        except Exception as disconnect_err:
            logger.warning(f"Error while disconnecting: {disconnect_err}")
        _unreal_connection = None
    sys.exit(exit_code)

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    logger.info("UnrealMCP server starting up (lazy Unreal connection)")
    # Do not attempt to connect to Unreal here; connect lazily on demand.
    _unreal_connection = None

    try:
        yield {}
    finally:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal MCP server shut down")

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    lifespan=server_lifespan
)

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools

# Register tools
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)  

@mcp.prompt()
def info():
    """Information about available Unreal MCP tools and best practices."""
    return """
    # Unreal MCP Server Tools and Best Practices
    
    ## UMG (Widget Blueprint) Tools
    - `create_umg_widget_blueprint(widget_name, parent_class="UserWidget", path="/Game/UI")` 
      Create a new UMG Widget Blueprint
    - `add_text_block_to_widget(widget_name, text_block_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1])`
      Add a Text Block widget with customizable properties
    - `add_button_to_widget(widget_name, button_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1], background_color=[0.1,0.1,0.1,1])`
      Add a Button widget with text and styling
    - `bind_widget_event(widget_name, widget_component_name, event_name, function_name="")`
      Bind events like OnClicked to functions
    - `add_widget_to_viewport(widget_name, z_order=0)`
      Add widget instance to game viewport
    - `set_text_block_binding(widget_name, text_block_name, binding_property, binding_type="Text")`
      Set up dynamic property binding for text blocks

    ## Editor Tools
    ### Viewport and Screenshots
    - `focus_viewport(target, location, distance, orientation)` - Focus viewport
    - `take_screenshot(filename, show_ui, resolution)` - Capture screenshots

    ### Actor Management
    - `get_actors_in_level()` - List all actors in current level
    - `find_actors_by_name(pattern)` - Find actors by name pattern
    - `spawn_actor(name, type, location=[0,0,0], rotation=[0,0,0], scale=[1,1,1])` - Create actors
    - `delete_actor(name)` - Remove actors
    - `set_actor_transform(name, location, rotation, scale)` - Modify actor transform
    - `get_actor_properties(name)` - Get actor properties
    
    ## Blueprint Management
    - `create_blueprint(name, parent_class)` - Create new Blueprint classes
    - `add_component_to_blueprint(blueprint_name, component_type, component_name)` - Add components
    - `set_static_mesh_properties(blueprint_name, component_name, static_mesh)` - Configure meshes
    - `set_physics_properties(blueprint_name, component_name)` - Configure physics
    - `compile_blueprint(blueprint_name)` - Compile Blueprint changes
    - `set_blueprint_property(blueprint_name, property_name, property_value)` - Set properties
    - `set_pawn_properties(blueprint_name)` - Configure Pawn settings
    - `spawn_blueprint_actor(blueprint_name, actor_name)` - Spawn Blueprint actors
    
    ## Blueprint Node Management
    - `add_blueprint_event_node(blueprint_name, event_type)` - Add event nodes
    - `add_blueprint_input_action_node(blueprint_name, action_name)` - Add input nodes
    - `add_blueprint_function_node(blueprint_name, target, function_name)` - Add function nodes
    - `connect_blueprint_nodes(blueprint_name, source_node_id, source_pin, target_node_id, target_pin)` - Connect nodes
    - `add_blueprint_variable(blueprint_name, variable_name, variable_type)` - Add variables
    - `add_blueprint_get_self_component_reference(blueprint_name, component_name)` - Add component refs
    - `add_blueprint_self_reference(blueprint_name)` - Add self references
    - `find_blueprint_nodes(blueprint_name, node_type, event_type)` - Find nodes
    
    ## Project Tools
    - `create_input_mapping(action_name, key, input_type)` - Create input mappings
    
    ## Best Practices
    
    ### UMG Widget Development
    - Create widgets with descriptive names that reflect their purpose
    - Use consistent naming conventions for widget components
    - Organize widget hierarchy logically
    - Set appropriate anchors and alignment for responsive layouts
    - Use property bindings for dynamic updates instead of direct setting
    - Handle widget events appropriately with meaningful function names
    - Clean up widgets when no longer needed
    - Test widget layouts at different resolutions
    
    ### Editor and Actor Management
    - Use unique names for actors to avoid conflicts
    - Clean up temporary actors
    - Validate transforms before applying
    - Check actor existence before modifications
    - Take regular viewport screenshots during development
    - Keep the viewport focused on relevant actors during operations
    
    ### Blueprint Development
    - Compile Blueprints after changes
    - Use meaningful names for variables and functions
    - Organize nodes logically
    - Test functionality in isolation
    - Consider performance implications
    - Document complex setups
    
    ### Error Handling
    - Check command responses for success
    - Handle errors gracefully
    - Log important operations
    - Validate parameters
    - Clean up resources on errors
    """

# Run the server
if __name__ == "__main__":
    logger.info("Starting MCP server")

    # Handle Ctrl+C cleanly
    signal.signal(signal.SIGINT, lambda *_: shutdown_server(0))

    try:
        if hasattr(mcp, "run_tcp"):
            logger.info(f"Starting MCP TCP transport on {MCP_TCP_HOST}:{MCP_TCP_PORT}")
            mcp.run_tcp(host=MCP_TCP_HOST, port=MCP_TCP_PORT)
        else:
            logger.warning("FastMCP 'run_tcp' not available; starting HTTP transport instead")
            mcp.run(transport="http", host=MCP_TCP_HOST, port=MCP_TCP_PORT)
    except KeyboardInterrupt:
        shutdown_server(0)
