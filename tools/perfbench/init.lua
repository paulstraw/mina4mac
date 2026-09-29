-- mina4mac performance benchmark (tools/perfbench.sh installs and runs it). Pins the world seed, protects the
-- player, floods the start area with water, oil and lava (fire, steam, lots of moving cells), then records the
-- real time of every frame and writes "PERFBENCH ..." lines to perfbench.txt in the game directory.
-- Scene "heavy" (mods/perfbench/scene.txt, written by perfbench.sh) doubles the flood with acid and more water and
-- oil, drops a grid of physics props (crates, barrels, explosive boxes) into it, adds four more seas every 10 s
-- and sets off two TNT boxes every 2.5 s (game time).
-- Scene "jungle" (for busy, mob-heavy scenes, PLAN4) teleports the player into the Underground Jungle, checks the
-- biome there (a wrong spot writes "fail ..." and ends the run), spawns waves of jungle enemies around the player
-- and sets off fire and TNT at fixed frames. It starts measuring later than the other scenes, after the world
-- around the new position has streamed in.
-- The start and end of the measured frames are also marked in perfbench_frames.txt, which mina4mac's
-- MINA4MAC_FRAMELOG appends per-frame CPU times to.
-- Start it with `-no_logo_splashes -gamemode 0` so no menu clicks are needed; don't touch the controls.
ModMagicNumbersFileAdd("mods/perfbench/files/magic_numbers.xml")

-- Release builds drop print() output, so the lines go to a file (needs request_no_api_restrictions in mod.xml and
-- mods_sandbox_enabled="0").
local file = io and io.open("perfbench.txt", "a")
local function out(s)
	if file then file:write("PERFBENCH ", s, "\n") ; file:flush() else print("PERFBENCH " .. s) end
end
local scene = "flood"
local sf = io and io.open("mods/perfbench/scene.txt", "r")
if sf then scene = sf:read("*l") or scene ; sf:close() end
out("init " .. (file and "file" or "no io") .. " scene " .. scene)

local function mark(s)
	local f = io and io.open("perfbench_frames.txt", "a")
	if f then f:write("mark ", s, "\n") ; f:close() end
end

local LOAD_AT = 60        -- frames after spawn: start the flood
local WARMUP = 120        -- frames after spawn: start measuring
-- jungle: teleport at LOAD_AT, move to open space at SETTLE_AT, first enemy wave at MOBS_AT
local SETTLE_AT, MOBS_AT = 180, 200
local JUNGLE_X, JUNGLE_Y = -768, 6912  -- biome map pixel (33, 27): Underground Jungle (data/biome/rainforest.xml)
if scene == "jungle" then WARMUP = 360 end
local FRAMES = 1800       -- frames measured (30 s at 60 fps)

local spawn_frame, px, py
local times = {}

function OnPlayerSpawned(player)
	px, py = EntityGetTransform(player)
	spawn_frame = GameGetFrameNum()
	out(string.format("player %.2f,%.2f frame %d", px, py, spawn_frame))
end

-- jungle: an open spot near (x, y) where the player fits (Raytrace is true on a solid cell), nearest first
local function open_spot(x, y)
	local function clear(cx, cy)
		for dy = -24, 8, 8 do
			if Raytrace(cx - 12, cy + dy, cx + 12, cy + dy) then return false end
		end
		return not Raytrace(cx, cy - 24, cx, cy + 8)
	end
	for r = 0, 480, 16 do
		for i = -r, r, 16 do
			for _, p in ipairs({ { x + i, y - r }, { x + i, y + r }, { x - r, y + i }, { x + r, y + i } }) do
				if clear(p[1], p[2]) then return p[1], p[2] end
			end
		end
	end
end

local jungle_mobs = { "rainforest/scavenger_smg", "rainforest/scavenger_grenade", "rainforest/scavenger_heal",
	"rainforest/scavenger_leader", "rainforest/scavenger_clusterbomb", "rainforest/scavenger_poison",
	"rainforest/scavenger_mine", "rainforest/shooterflower", "rainforest/fly", "rainforest/bloom",
	"rainforest/fungus", "rainforest/flamer", "rainforest/coward", "rainforest/sniper", "lukki/lukki",
	"shaman", "wizard_twitchy", "spearbot" }
local function jungle_wave(count, first)
	for i = 0, count - 1 do
		local name = jungle_mobs[(first + i) % #jungle_mobs + 1]
		local a = (i + 0.5) / count * 2 * math.pi
		local d = 120 + (i % 3) * 60
		EntityLoad("data/entities/animals/" .. name .. ".xml", px + math.cos(a) * d, py + math.sin(a) * d * 0.5)
	end
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
	if n == LOAD_AT and scene ~= "jungle" then
		local player = EntityGetWithTag("player_unit")[1]
		if player then LoadGameEffectEntityTo(player, "data/entities/misc/effect_protection_all.xml") end
		EntityLoad("data/entities/projectiles/deck/sea_water.xml", px - 200, py - 250)
		EntityLoad("data/entities/projectiles/deck/sea_oil.xml", px + 150, py - 250)
		EntityLoad("data/entities/projectiles/deck/sea_lava.xml", px + 450, py - 250)
		if scene == "heavy" then
			EntityLoad("data/entities/projectiles/deck/sea_acid.xml", px - 450, py - 250)
			EntityLoad("data/entities/projectiles/deck/sea_water.xml", px + 700, py - 300)
			EntityLoad("data/entities/projectiles/deck/sea_oil.xml", px - 700, py - 300)
			local props = { "physics_crate", "physics_barrel_oil", "physics_box_harmless", "physics_box_explosive",
				"physics_barrel_water", "physics_propane_tank" }
			for row = 0, 3 do
				for col = 0, 11 do
					local name = props[(row * 12 + col) % #props + 1]
					EntityLoad("data/entities/props/" .. name .. ".xml", px - 550 + col * 100, py - 180 - row * 40)
				end
			end
		end
		out("load")
	end
	if scene == "jungle" then
		local player = EntityGetWithTag("player_unit")[1]
		if n == LOAD_AT and player then
			LoadGameEffectEntityTo(player, "data/entities/misc/effect_protection_all.xml")
			px, py = JUNGLE_X, JUNGLE_Y
			EntitySetTransform(player, px, py)
			out(string.format("teleport %.0f,%.0f biome %s", px, py, BiomeMapGetName(px, py)))
		elseif n == SETTLE_AT and player then
			local x, y = open_spot(JUNGLE_X, JUNGLE_Y)
			local biome = x and BiomeMapGetName(x, y)
			if not x or biome ~= "$biome_rainforest" then
				out(string.format("fail open spot %s biome %s", tostring(x), tostring(biome)))
				out("done") ; spawn_frame = nil ; return
			end
			px, py = x, y
			EntitySetTransform(player, px, py)
			out(string.format("spot %.0f,%.0f biome %s", px, py, biome))
		elseif n >= MOBS_AT and n <= WARMUP + FRAMES then
			local t = n - MOBS_AT
			if t % 300 == 0 then jungle_wave(t == 0 and 30 or 12, t / 60) end  -- 30 enemies, then 12 more every 5 s
			if t % 150 == 75 then  -- fire in the plants on alternating sides, and TNT
				local side = (t / 150) % 2 == 0 and -1 or 1
				EntityLoad("data/entities/projectiles/deck/circle_fire.xml", px + side * 200, py - 40)
				EntityLoad("data/entities/projectiles/deck/tntbox.xml", px - side * 250, py - 100)
			end
			if t == 0 then out("load") end
		end
	end
	if scene == "heavy" and n > LOAD_AT and n <= WARMUP + FRAMES then
		if (n - LOAD_AT) % 600 == 0 then  -- keep the flood going: more of every sea
			for i, sea in ipairs({ "water", "oil", "lava", "acid" }) do
				EntityLoad("data/entities/projectiles/deck/sea_" .. sea .. ".xml", px - 600 + i * 240, py - 300)
			end
		end
		if (n - LOAD_AT) % 150 == 0 then
			EntityLoad("data/entities/projectiles/deck/tntbox.xml", px - 300, py - 150)
			EntityLoad("data/entities/projectiles/deck/tntbox.xml", px + 300, py - 150)
		end
	end
	if n >= WARMUP and n <= WARMUP + FRAMES then
		if n == WARMUP then mark("start") end
		times[#times + 1] = GameGetRealWorldTimeSinceStarted()
		if (n - WARMUP) % 300 == 0 then  -- with the entities (and enemies) within 1024 px of the scene's centre
			out(string.format("frame +%d at %.3f s entities %d enemies %d", n, times[#times],
				#(EntityGetInRadius(px, py, 1024) or {}), #(EntityGetInRadiusWithTag(px, py, 1024, "enemy") or {})))
		end
		if n == WARMUP + FRAMES then mark("end") ; report() ; spawn_frame = nil end
	end
end
