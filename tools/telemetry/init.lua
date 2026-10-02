-- mina4mac playtest telemetry (tools/package_app.sh --telemetry installs it). Once a second of game time, hands a
-- line with the frame, the biome and position of the player and, within 1024 px, the entities and enemies (as the
-- perfbench scenes count them), the entities with a physics body and the projectiles to mina4mac_telemetry, a host
-- function of mina4mac's Lua bridge (runtime/lua51.c), which appends it to the [fps] lines of
-- ~/Library/Logs/mina4mac.log. A polymorphed player is found by its polymorphed_player tag and marked "poly".
-- Read-only: changes nothing in the game. In other builds (Windows, Wine) the function doesn't exist and the mod
-- does nothing.
function OnWorldPostUpdate()
	if not mina4mac_telemetry or GameGetFrameNum() % 60 ~= 0 then return end
	local player, poly = EntityGetWithTag("player_unit")[1], ""
	if not player then
		player, poly = EntityGetWithTag("polymorphed_player")[1], " poly"
	end
	if not player then
		mina4mac_telemetry(string.format("f %d, no player", GameGetFrameNum()))
		return
	end
	local x, y = EntityGetTransform(player)
	local near = EntityGetInRadius(x, y, 1024) or {}
	local bodies = 0
	for _, e in ipairs(near) do
		if EntityGetFirstComponent(e, "PhysicsBodyComponent") or EntityGetFirstComponent(e, "PhysicsBody2Component") then
			bodies = bodies + 1
		end
	end
	mina4mac_telemetry(string.format("f %d, %s at %.0f,%.0f%s, entities %d, enemies %d, bodies %d, projectiles %d",
		GameGetFrameNum(), BiomeMapGetName(x, y), x, y, poly, #near,
		#(EntityGetInRadiusWithTag(x, y, 1024, "enemy") or {}), bodies,
		#(EntityGetInRadiusWithTag(x, y, 1024, "projectile") or {})))
end
