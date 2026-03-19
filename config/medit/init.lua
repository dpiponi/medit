local function home_path(relative)
  local home = os.getenv("HOME")
  if not home or home == "" then
    return relative
  end
  return home .. "/" .. relative
end

local function show_status_summary()
  local status = medit.status()
  medit.set_status(
    string.format(
      "mode=%s buffer=%d window=%d buffers=%d windows=%d",
      status.mode,
      status.active_buffer_id,
      status.active_window_id,
      status.buffer_count,
      status.window_count
    )
  )
end

local function edit_lua_init()
  medit.open_file(home_path(".config/medit/init.lua"))
  medit.set_status("Opened Lua init")
end

medit.register_command("lua-status", show_status_summary)
medit.register_command("edit-lua-init", edit_lua_init)
