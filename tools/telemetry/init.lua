-- mina4mac playtest telemetry (tools/package_app.sh --telemetry installs it). Once a second of game time, hands a
-- line with the frame, the biome and position of the player and the entities and enemies within 1024 px (as the
-- perfbench scenes count them) to mina4mac_telemetry, a host function of mina4mac's Lua bridge (runtime/lua51.c),
-- which appends it to the [fps] lines of ~/Library/Logs/mina4mac.log. Read-only: changes nothing in the game. In
-- other builds (Windows, Wine) the function doesn't exist and the mod does nothing.
function OnWorldPostUpdate()
	if not mina4mac_telemetry or GameGetFrameNum() % 60 ~= 0 then return end
	local player = EntityGetWithTag("player_unit")[1]
	if not player then
		mina4mac_telemetry(string.format("f %d, no player", GameGetFrameNum()))
		return
	end
	local x, y = EntityGetTransform(player)
	mina4mac_telemetry(string.format("f %d, %s at %.0f,%.0f, entities %d, enemies %d", GameGetFrameNum(),
		BiomeMapGetName(x, y), x, y, #(EntityGetInRadius(x, y, 1024) or {}),
		#(EntityGetInRadiusWithTag(x, y, 1024, "enemy") or {})))
end
