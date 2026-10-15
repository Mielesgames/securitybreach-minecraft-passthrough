package dev.skycraft.client;

import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyTri;
import dev.skycraft.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/** Feeds the local player's movement through {@link TriCollider} against nearby Skyrim triangles. */
public final class SkyCollider {
	private SkyCollider() {
	}

	public static Vec3 collide(LocalPlayer player, Vec3 move) {
		AABB box = player.getBoundingBox();
		move = intoKnown(box, move);
		double step = player.maxUpStep();
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(box.expandTowards(move).inflate(1.0, 1.0 + step, 1.0), tris);
		if (tris.isEmpty()) {
			return move;
		}
		// Security Breach's walls arrive rounded up to eighth-block cells, which makes every doorway
		// up to an eighth of a block narrower on each side. A slightly slimmer body makes up for it.
		double radius = SkyClient.passthrough() ? 0.25 : box.getXsize() * 0.5;
		double[] r = TriCollider.resolve(
			tris, (box.minX + box.maxX) * 0.5, box.minY, (box.minZ + box.maxZ) * 0.5, radius, box.getYsize(), step, player.onGround(),
			move.x, move.y, move.z
		);
		if (r[0] == move.x && r[1] == move.y && r[2] == move.z) {
			return move;
		}
		// The triangle pass (snapping down a slope, pushing out of a wall) can move the player into a
		// Minecraft block placed on the terrain; collide that result with Minecraft blocks again.
		return Entity.collideBoundingBox(player, new Vec3(r[0], r[1], r[2]), box, player.level(), List.of());
	}

	/**
	 * Security Breach: the game's walls and floors reach Minecraft a region at a time, scanned
	 * around the player. Flying fast (creative), the player outran the scan and dropped through a
	 * floor that wasn't there yet (15 Oct). Like waiting for a chunk to load, the player doesn't
	 * go where nothing is known yet: each direction that would is held back until it arrives.
	 */
	/** For what the player rides: the same wait at the edge of what is known. */
	public static Vec3 vehicleIntoKnown(net.minecraft.world.entity.Entity vehicle, Vec3 move) {
		return intoKnown(vehicle.getBoundingBox(), move);
	}

	private static Vec3 intoKnown(AABB box, Vec3 move) {
		if (!SkyClient.passthrough() || !SkyCollision.active() || SkyCollision.isKnown(box.move(move))) {
			return move;
		}
		double x = SkyCollision.isKnown(box.move(move.x, 0, 0)) ? move.x : 0;
		double y = SkyCollision.isKnown(box.move(x, move.y, 0)) ? move.y : 0;
		double z = SkyCollision.isKnown(box.move(x, y, move.z)) ? move.z : 0;
		if (System.currentTimeMillis() - heldLogged > 5000) {
			heldLogged = System.currentTimeMillis();
			dev.skycraft.SkyCraft.LOG.info("FNAFSBCraft: held back at the edge of the scanned area (the game's geometry there hasn't arrived yet)");
		}
		return new Vec3(x, y, z);
	}

	private static long heldLogged;

	/** Highest Skyrim surface at or below {@code maxAbove} over the feet at (x, y, z), or NaN. */
	public static double groundAt(double x, double y, double z, double maxAbove) {
		List<SkyTri> tris = new ArrayList<>();
		SkyCollision.trianglesNear(new AABB(x - 1, y - 4, z - 1, x + 1, y + maxAbove + 1, z + 1), tris);
		return TriCollider.groundAt(tris, x, y, z, maxAbove);
	}
}
