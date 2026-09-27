-- mina4mac determinism check (tools/determinism.sh installs it). Pins the world seed and prints a fingerprint
-- of the start area, as "SEEDPRINT ..." lines, at fixed frames after the player spawns. Diff the lines from
-- two builds (tools/determinism.sh diff). Don't touch the controls until the "SEEDPRINT done" line (~10 s).
ModMagicNumbersFileAdd("mods/seedprint/files/magic_numbers.xml")

-- Release builds drop print() output, so the lines go to seedprint.txt in the game directory (this needs the
-- unrestricted API: request_no_api_restrictions in mod.xml, and mods_sandbox_enabled="0").
local file = io and io.open("seedprint.txt", "a")
local function out(s)
	if file then file:write("SEEDPRINT ", s, "\n") ; file:flush() else print("SEEDPRINT " .. s) end
end
out("init " .. (file and "file" or "no io"))
local function num(x) return string.format("%.9g", x) end

-- Engine and Lua arithmetic that doesn't depend on the world: RNGs and libm.
function OnMagicNumbersAndWorldSeedInitialized()
	out("seed " .. tostring(StatsGetValue("world_seed")))
	local r = {}
	for i = 0, 7 do r[#r + 1] = num(ProceduralRandomf(i * 37.5, -i * 11)) end
	out("procedural " .. table.concat(r, " "))
	SetRandomSeed(12.5, -3)
	r = {}
	for i = 1, 8 do r[#r + 1] = num(Randomf()) end
	out("random " .. table.concat(r, " "))
	r = {}
	for _, x in ipairs({0.1, 1.2345, 3.3, 100.7, -2.5}) do
		r[#r + 1] = string.format("%.17g/%.17g/%.17g/%.17g/%.17g", math.sin(x), math.cos(x), math.exp(x / 4),
			math.pow(math.abs(x), 1.37), math.atan2(x, 1.7))
	end
	out("libm " .. table.concat(r, " "))
end

-- The cell under (x, y), coarsely: 0 empty or gas/fire, 1 liquid-ish, 2 other non-standable, 3 standable.
local function cell(x, y)
	if not Raytrace(x, y, x, y + 0.5) then return 0 end
	if not RaytraceSurfaces(x, y, x, y + 0.5) then return 1 end
	if not RaytracePlatforms(x, y, x, y + 0.5) then return 2 end
	return 3
end

local function snapshot(tag, px, py)
	-- 128x128 grid, 4 px apart, centred on the spawn point; one line per row.
	local x0, y0 = math.floor(px) - 256, math.floor(py) - 256
	for j = 0, 127 do
		local row = {}
		for i = 0, 127 do row[#row + 1] = cell(x0 + 4 * i, y0 + 4 * j) end
		out(tag .. " grid " .. j .. " " .. table.concat(row))
	end
	-- Top-level entities within 1024 px, sorted.
	local list = {}
	for _, e in ipairs(EntityGetInRadius(px, py, 1024) or {}) do
		if EntityGetParent(e) == 0 then
			local x, y = EntityGetTransform(e)
			list[#list + 1] = string.format("%s|%s|%.1f,%.1f", EntityGetFilename(e), EntityGetName(e), x, y)
		end
	end
	table.sort(list)
	out(tag .. " entities " .. #list)
	for _, s in ipairs(list) do out(tag .. " entity " .. s) end
end

local spawn_frame, px, py
local SNAPSHOTS = {[1] = "f1", [60] = "f60", [600] = "f600"}

function OnPlayerSpawned(player)
	px, py = EntityGetTransform(player)
	spawn_frame = GameGetFrameNum()
	out(string.format("player %.2f,%.2f frame %d", px, py, spawn_frame))
end

function OnWorldPostUpdate()
	if not spawn_frame then return end
	local n = GameGetFrameNum() - spawn_frame
	if SNAPSHOTS[n] then snapshot(SNAPSHOTS[n], px, py) end
	if n == 600 then out("done") ; spawn_frame = nil end
end
