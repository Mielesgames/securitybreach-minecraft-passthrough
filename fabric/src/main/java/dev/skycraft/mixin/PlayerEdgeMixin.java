package dev.skycraft.mixin;

import dev.skycraft.link.SkyLink;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Crouching and edges. Minecraft looks for block collision under the player to decide where an edge
 * is; the Skyrim ground the player walks on isn't blocks (the player collides with its exact
 * triangles), so every direction looked like a drop and crouching froze the player: there, crouching
 * walks anywhere. Security Breach's floors are known per block as well (SkyCollision's fill), so
 * there crouching stops at edges again, like Minecraft's, against the game's floors and blocks both.
 */
@Mixin(Player.class)
public abstract class PlayerEdgeMixin {
	@Inject(method = "maybeBackOffFromEdge", at = @At("HEAD"), cancellable = true)
	private void skycraft$crouchWalkAnywhere(Vec3 delta, MoverType moverType, CallbackInfoReturnable<Vec3> cir) {
		if (!SkyLink.active()) {
			return;
		}
		Player player = (Player) (Object) this;
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || !player.isShiftKeyDown() || player.getAbilities().flying || !player.onGround()
			|| delta.y > 0.0 || (moverType != MoverType.SELF && moverType != MoverType.PLAYER)) {
			cir.setReturnValue(delta);
			return;
		}
		// Minecraft's own back-off: shorten the step until there is ground under the moved body.
		double x = delta.x;
		double z = delta.z;
		final double step = 0.05;
		while (x != 0.0 && !skycraft$supported(player, x, 0.0)) {
			x = Math.abs(x) <= step ? 0.0 : x - Math.copySign(step, x);
		}
		while (z != 0.0 && !skycraft$supported(player, 0.0, z)) {
			z = Math.abs(z) <= step ? 0.0 : z - Math.copySign(step, z);
		}
		while (x != 0.0 && z != 0.0 && !skycraft$supported(player, x, z)) {
			x = Math.abs(x) <= step ? 0.0 : x - Math.copySign(step, x);
			z = Math.abs(z) <= step ? 0.0 : z - Math.copySign(step, z);
		}
		cir.setReturnValue(new Vec3(x, delta.y, z));
	}

	/** Ground within a step below the body moved by (dx, dz): a Minecraft block or the game's floor. */
	private static boolean skycraft$supported(Player player, double dx, double dz) {
		var box = player.getBoundingBox().move(dx, 0.0, dz);
		if (!player.level().noCollision(player, box.move(0.0, -player.maxUpStep(), 0.0).deflate(1.0E-7, 0.0, 1.0E-7))) {
			return true;
		}
		int top = (int) Math.floor(box.minY - 0.001);
		int bottom = (int) Math.floor(box.minY - player.maxUpStep());
		for (double cx : new double[] { box.minX + 0.01, box.maxX - 0.01 }) {
			for (double cz : new double[] { box.minZ + 0.01, box.maxZ - 0.01 }) {
				for (int y = top; y >= bottom; y--) {
					if (dev.skycraft.world.SkyCollision.hasGeometry(net.minecraft.core.BlockPos.containing(cx, y, cz))) {
						return true;
					}
				}
			}
		}
		return false;
	}
}
