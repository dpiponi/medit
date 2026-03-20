---@class MeditStatus
---@field mode string
---@field active_buffer_id integer
---@field active_window_id integer
---@field buffer_count integer
---@field window_count integer
---@field status_message string

---@class MeditEventPosition
---@field row integer
---@field column integer

---@class MeditEventRange
---@field start MeditEventPosition
---@field ["end"] MeditEventPosition

---@class MeditEvent
---@field type string
---@field document_uri string
---@field document_version integer
---@field cursor MeditEventPosition
---@field range MeditEventRange|nil
---@field text string

---@class medit
local M = {}

---@return MeditStatus
function M.status() end

---@return MeditEventPosition
function M.get_cursor() end

---@return MeditEventRange|nil
function M.get_selection() end

---@return string|nil
function M.get_selection_text() end

---@return string
function M.get_buffer_text() end

---@param row integer
---@return string
function M.get_line_text(row) end

---@param range MeditEventRange
---@return string
function M.get_text(range) end

---@param position MeditEventPosition
function M.set_cursor(position) end

---@param text string
---@return boolean
function M.replace_selection(text) end

---@param text string
---@return boolean
function M.replace_buffer(text) end

---@param range MeditEventRange
---@param text string
---@return boolean
function M.replace_range(range, text) end

---@param message string
function M.set_status(message) end

---@return string
function M.current_working_directory() end

---@return string|nil
function M.current_file_path() end

---@return string|nil
function M.workspace_root() end

---@return string|nil
function M.token_under_cursor() end

---@param title string
---@param text string
function M.show_popup(title, text) end

---@return string[]
function M.list_themes() end

---@param key string
---@param value string
function M.set_config_value(key, value) end

function M.reload_config() end

---@param path string
function M.open_file(path) end

---@param path string
---@param position MeditEventPosition
function M.open_location(path, position) end

---@param name string
---@return boolean
function M.executable_exists(name) end

---@param path string
---@return boolean
function M.file_exists(path) end

---@param text string
---@return string
function M.shell_quote(text) end

---@param command string
---@return string|nil selection
---@return string|nil error
function M.run_picker(command) end

---@param command string
---@param input string
---@return string|nil output
---@return string|nil error
function M.run_filter(command, input) end

---@return string|nil
function M.resolve_ai_command() end

---@return string
function M.resolve_ai_provider() end

---@param provider string|nil
---@return string
function M.resolve_ai_model(provider) end

---@param name string
---@param fn fun(argument: string)
function M.register_command(name, fn) end

---@param name string
---@param fn fun(): string
function M.register_health_check(name, fn) end

---@param event_name string
---@param fn fun(event: MeditEvent)
function M.on(event_name, fn) end

return M
