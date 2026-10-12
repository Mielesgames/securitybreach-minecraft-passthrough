package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyDig;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Mobs find their way over the host game's floors (Security Breach). To the pathfinder those are
 * air: mobs stood on them, since collision is injected, but saw nothing to walk on. A cell counts
 * as solid when the host's geometry fills its upper half, or when it carries a floor that lies in
 * the lower half of the cell above; Minecraft then works out the walkable cells itself.
 */
@Mixin(WalkNodeEvaluator.class)
public abstract class WalkNodeEvaluatorMixin {
	@Inject(method = "getPathTypeFromState", at = @At("RETURN"), cancellable = true)
	private static void skycraft$hostGround(BlockGetter level, BlockPos pos, CallbackInfoReturnable<PathType> cir) {
		if (!SkyDig.PASSTHROUGH || cir.getReturnValue() != PathType.OPEN || !SkyCollision.active()) {
			return;
		}
		if ((SkyCollision.hasGeometry(pos) && SkyCollision.groundTop(pos) > 0.5F) || SkyCollision.supportsFromBelow(pos.above())) {
			cir.setReturnValue(PathType.BLOCKED);
		}
	}
}
