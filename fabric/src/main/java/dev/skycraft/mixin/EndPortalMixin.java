package dev.skycraft.mixin;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.InsideBlockEffectApplier;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.EndPortalBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Security Breach easter egg: an end portal doesn't lead to the End, it ends the game the Minecraft
 * way: the client shows the ending (EndingScreen) and the host plays the music under it.
 */
@Mixin(EndPortalBlock.class)
public abstract class EndPortalMixin {
	@Unique
	private static long skycraft$nextCredits;

	/** Next to the portal: the nearest free cell around it that isn't portal or frame. */
	@Unique
	private static void skycraft$stepOut(Level level, BlockPos pos, ServerPlayer player) {
		BlockPos best = null;
		double bestDistance = Double.MAX_VALUE;
		for (int dx = -4; dx <= 4; dx++) {
			for (int dz = -4; dz <= 4; dz++) {
				for (int dy = 0; dy <= 1; dy++) {
					var cell = pos.offset(dx, dy, dz);
					var feet = level.getBlockState(cell);
					if (feet.is(net.minecraft.world.level.block.Blocks.END_PORTAL) || feet.is(net.minecraft.world.level.block.Blocks.END_PORTAL_FRAME)
						|| !feet.canBeReplaced() || !level.getBlockState(cell.above()).canBeReplaced()) {
						continue;
					}
					double distance = cell.distSqr(pos);
					if (distance < bestDistance) {
						bestDistance = distance;
						best = cell;
					}
				}
			}
		}
		if (best != null) {
			player.teleportTo(best.getX() + 0.5, best.getY(), best.getZ() + 0.5);
			SkyCraft.LOG.info("FNAFSBCraft: {} put next to the end portal, at {}", player.getName().getString(), best);
		}
	}

	@Inject(method = "entityInside", at = @At("HEAD"), cancellable = true)
	private void skycraft$credits(BlockState state, Level level, BlockPos pos, Entity entity, InsideBlockEffectApplier effects, boolean intersects,
		CallbackInfo ci) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || !(entity instanceof ServerPlayer player)) {
			return;
		}
		ci.cancel();
		// Just loaded in standing in it (the ending sends you to the title, and Minecraft keeps
		// you where you were): out of it, no ending again.
		if (player.tickCount < 100) {
			skycraft$stepOut(level, pos, player);
			return;
		}
		long now = level.getGameTime();
		if (now < skycraft$nextCredits) {
			return;
		}
		skycraft$nextCredits = now + 200;
		SkyLink.pushEvent(Proto.EV_CREDITS, 0, 0, 0, 0, 0, 0);
		SkyCraft.endingRequested = true;
		SkyCraft.award(level.getServer(), "the_end");
		skycraft$stepOut(level, pos, player);
		SkyCraft.LOG.info("FNAFSBCraft: {} stepped into an end portal: the ending plays", player.getName().getString());
	}
}
