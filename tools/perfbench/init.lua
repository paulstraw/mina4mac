-- noitamac performance benchmark (tools/perfbench.sh installs and runs it). Pins the world seed, protects the
-- player, floods the start area with water, oil and lava (fire, steam, lots of moving cells), then records the
-- real time of every frame and writes "PERFBENCH ..." lines to perfbench.txt in the game directory.
-- Start it with `-no_logo_splashes -gamemode 0` so no menu clicks are needed; don't touch the controls.
ModMagicNumbersFileAdd("mods/perfbench/files/magic_numbers.xml")

-- Release builds drop print() output, so the lines go to a file (needs request_no_api_restrictions in mod.xml and
-- mods_sandbox_enabled="0").
local file = io and io.open("perfbench.txt", "a")
local function out(s)
	if file then file:write("PERFBENCH ", s, "\n") ; file:flush() else print("PERFBENCH " .. s) end
end
out("init " .. (file and "file" or "no io"))

local LOAD_AT = 60        -- frames after spawn: start the flood
local WARMUP = 120        -- frames after spawn: start measuring
local FRAMES = 1800       -- frames measured (30 s at 60 fps)

local spawn_frame, px, py
local times = {}

function OnPlayerSpawned(player)
	px, py = EntityGetTransform(player)
	spawn_frame = GameGetFrameNum()
	out(string.format("player %.2f,%.2f frame %d", px, py, spawn_frame))
end

local function report()
	local dts = {}
	for i = 2, #times do dts[#dts + 1] = (times[i] - times[i - 1]) * 1000 end
	local total = times[#times] - times[1]
	table.sort(dts)
	local function pct(p) return dts[math.max(1, math.ceil(#dts * p))] end
	out(string.format("frames %d seconds %.3f fps %.2f", #dts, total, #dts / total))
	out(string.format("frame_ms median %.2f p90 %.2f p99 %.2f max %.2f", pct(0.5), pct(0.9), pct(0.99), dts[#dts]))
	-- fps per 5 s of game frames (300 frames), to show how the load develops
	local b = {}
	for i = 1, #times - 300, 300 do b[#b + 1] = string.format("%.1f", 300 / (times[i + 300] - times[i])) end
	out("fps_per_300_frames " .. table.concat(b, " "))
	out("done")
end

function OnWorldPostUpdate()
	if not spawn_frame then return end
	local n = GameGetFrameNum() - spawn_frame
	if n == LOAD_AT then
		local player = EntityGetWithTag("player_unit")[1]
		if player then LoadGameEffectEntityTo(player, "data/entities/misc/effect_protection_all.xml") end
		EntityLoad("data/entities/projectiles/deck/sea_water.xml", px - 200, py - 250)
		EntityLoad("data/entities/projectiles/deck/sea_oil.xml", px + 150, py - 250)
		EntityLoad("data/entities/projectiles/deck/sea_lava.xml", px + 450, py - 250)
		out("load")
	elseif n >= WARMUP and n <= WARMUP + FRAMES then
		times[#times + 1] = GameGetRealWorldTimeSinceStarted()
		if (n - WARMUP) % 300 == 0 then out(string.format("frame +%d at %.3f s", n, times[#times])) end
		if n == WARMUP + FRAMES then report() ; spawn_frame = nil end
	end
end
