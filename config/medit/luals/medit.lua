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
---@field buffer_id integer|nil
---@field cursor MeditEventPosition
---@field range MeditEventRange|nil
---@field text string

---@class MeditJobSpec
---@field command string
---@field buffer_id integer|nil
---@field on_exit fun(job_id: integer, exit_code: integer)|nil

---@class MeditJobStatus
---@field running boolean
---@field exit_code integer
---@field command string
---@field buffer_id integer|nil

---@class MeditProcessSpec
---@field command string
---@field buffer_id integer|nil
---@field on_stdout fun(process_id: integer, text: string)|nil
---@field on_stderr fun(process_id: integer, text: string)|nil
---@field on_exit fun(process_id: integer, exit_code: integer)|nil

---@class MeditProcessStatus
---@field running boolean
---@field exit_code integer
---@field command string
---@field buffer_id integer|nil

---@class MeditAnnotationStyle
---@field foreground string|nil
---@field background string|nil
---@field bold boolean|nil
---@field underline boolean|nil
---@field reverse boolean|nil

---@class MeditLineAnnotation
---@field line integer
---@field text string
---@field severity "info"|"warning"|"error"|nil
---@field source string|nil
---@field style MeditAnnotationStyle|nil

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

---@param items MeditLineAnnotation[]
---@param buffer_id integer|nil
function M.set_line_annotations(items, buffer_id) end

---@param buffer_id integer|nil
function M.clear_line_annotations(buffer_id) end

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

---@param name string
---@param kind string|nil
---@return integer
function M.create_buffer(name, kind) end

---@param buffer_id integer
---@param text string
function M.append_buffer(buffer_id, text) end

---@param buffer_id integer
function M.clear_buffer(buffer_id) end

---@param buffer_id integer
---@param focus_panel boolean|nil
function M.show_buffer_in_panel(buffer_id, focus_panel) end

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

---@param value string
---@return boolean
function M.theme_color_supported(value) end

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

---@param spec MeditJobSpec
---@return integer
function M.job_start(spec) end

---@param job_id integer
---@return MeditJobStatus|nil
function M.job_status(job_id) end

---@param spec MeditProcessSpec
---@return integer
function M.process_start(spec) end

---@param process_id integer
---@param text string
---@return boolean
function M.process_send(process_id, text) end

---@param process_id integer
function M.process_close_stdin(process_id) end

---@param process_id integer
---@return MeditProcessStatus|nil
function M.process_status(process_id) end

---@param process_id integer
function M.process_stop(process_id) end

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
