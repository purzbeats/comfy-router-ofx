-- Comfy Router - Import Generated Media
--
-- Imports every Comfy Router generation that isn't in the Media Pool yet into a
-- "Comfy Router" bin: stills (GPT Image 2.5 PNGs keep their alpha) and Seedance MP4s
-- (with audio). Safe to run repeatedly — files already in the pool are skipped.
--
-- Installed automatically by the Comfy Router OFX plugin into Resolve's user Scripts
-- folder, so it shows up under Workspace → Scripts. Works in Resolve and Resolve Studio.
-- The plugin's "Import Generated Media" button runs this same file through fuscript
-- (Studio, with Preferences → General → External scripting set to Local).

local BIN_NAME = "Comfy Router"
local MEDIA_EXT = { png = true, jpg = true, jpeg = true, webp = true, mp4 = true, mov = true }

local sep = package.config:sub(1, 1)
local isWindows = sep == "\\"
local home = os.getenv("HOME") or os.getenv("USERPROFILE") or ""

local function join(a, b) return a .. sep .. b end

local function configDir()
  if isWindows then
    return join(os.getenv("APPDATA") or join(home, "AppData\\Roaming"), "ComfyRouterOFX")
  end
  local f = io.open("/System/Library/CoreServices/SystemVersion.plist", "r")  -- macOS?
  if f then
    f:close()
    return home .. "/Library/Application Support/ComfyRouterOFX"
  end
  local xdg = os.getenv("XDG_CONFIG_HOME")
  return (xdg and xdg ~= "" and xdg or home .. "/.config") .. "/comfy-router-ofx"
end

local function defaultOutputDir()
  local f = io.open("/System/Library/CoreServices/SystemVersion.plist", "r")
  if f then
    f:close()
    return home .. "/Movies/ComfyRouter"
  end
  return join(join(home, "Videos"), "ComfyRouter")
end

-- The plugin reads this back to show the outcome on its Status line.
local function report(msg)
  local f = io.open(join(configDir(), "last_import.txt"), "w")
  if f then
    f:write(msg)
    f:close()
  end
  print("Comfy Router: " .. msg)
end

local function norm(p)
  p = p:gsub("\\", "/")
  if isWindows then p = p:lower() end
  return p
end

local function count(t)
  local n = 0
  for _ in pairs(t or {}) do n = n + 1 end
  return n
end

local app = resolve
if not app and bmd and bmd.scriptapp then app = bmd.scriptapp("Resolve") end
if not app and Resolve then app = Resolve() end
if not app then
  report("error unreachable: Resolve scripting is not available here")
  return
end

local pm = app:GetProjectManager()
local project = pm and pm:GetCurrentProject()
if not project then
  report("error No project is open")
  return
end
local pool = project:GetMediaPool()
local storage = app:GetMediaStorage()

-- Folders to scan: every output folder the plugin has used, plus the default.
local dirs, seenDir = {}, {}
local function addDir(d)
  d = d and d:gsub("[\r\n]+$", "") or ""
  if d ~= "" and not seenDir[norm(d)] then
    seenDir[norm(d)] = true
    dirs[#dirs + 1] = d
  end
end
local list = io.open(join(configDir(), "output_dirs.txt"), "r")
if list then
  for line in list:lines() do addDir(line) end
  list:close()
end
addDir(defaultOutputDir())

-- Everything already in the pool, anywhere, so nothing is imported twice.
local have = {}
local function scan(folder)
  for _, clip in pairs(folder:GetClipList() or {}) do
    local p = clip:GetClipProperty("File Path")
    if type(p) == "string" and p ~= "" then have[norm(p)] = true end
  end
  for _, sub in pairs(folder:GetSubFolderList() or {}) do scan(sub) end
end
local root = pool:GetRootFolder()
scan(root)

local new = {}
for _, dir in ipairs(dirs) do
  for _, path in pairs(storage:GetFileList(dir) or {}) do
    if type(path) == "string" then
      local name = path:match("[^/\\]+$") or ""
      local ext = (name:match("%.([%w]+)$") or ""):lower()
      if name:sub(1, 12) == "ComfyRouter_" and MEDIA_EXT[ext] and not have[norm(path)] then
        new[#new + 1] = path
      end
    end
  end
end

if #new == 0 then
  report("ok 0")
  return
end
-- File names end in a timestamped job id, so sort by that to import oldest → newest.
table.sort(new, function(a, b)
  return (a:match("_(%d+%-%d+%-%a+)%.%w+$") or a) < (b:match("_(%d+%-%d+%-%a+)%.%w+$") or b)
end)

local bin
for _, sub in pairs(root:GetSubFolderList() or {}) do
  if sub:GetName() == BIN_NAME then bin = sub end
end
if not bin then bin = pool:AddSubFolder(root, BIN_NAME) end

local previous = pool:GetCurrentFolder()
if bin then pool:SetCurrentFolder(bin) end
local items = pool:ImportMedia(new)
if previous then pool:SetCurrentFolder(previous) end

local imported = count(items)
if imported == 0 then
  report("error Resolve refused the import (" .. #new .. " files)")
else
  report("ok " .. imported)
end
