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

---@param position MeditEventPosition
function M.set_cursor(position) end

---@param message string
function M.set_status(message) end

---@return string
function M.current_working_directory() end

---@param title string
---@param text string
function M.show_popup(title, text) end

---@param path string
function M.open_file(path) end

---@param path string
---@param position MeditEventPosition
function M.open_location(path, position) end

---@param name string
---@return boolean
function M.executable_exists(name) end

---@param text string
---@return string
function M.shell_quote(text) end

---@param command string
---@return string|nil selection
---@return string|nil error
function M.run_picker(command) end

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
