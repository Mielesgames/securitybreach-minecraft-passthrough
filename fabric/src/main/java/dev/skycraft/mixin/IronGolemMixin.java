package dev.skycraft.mixin;

import dev.skycraft.combat.SkyrimActorEntity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.animal.golem.AbstractGolem;
import net.minecraft.world.entity.animal.golem.IronGolem;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Iron golems go after the host game's characters (Security Breach's animatronics) the way they
 * go after monsters. What they hit is reported to the host like any other hit.
 */
@Mixin(IronGolem.class)
public abstract class IronGolemMixin extends AbstractGolem {
	protected IronGolemMixin(EntityType<? extends AbstractGolem> type, Level level) {
		super(type, level);
	}

	@Inject(method = "registerGoals", at = @At("TAIL"))
	private void skycraft$huntHostActors(CallbackInfo ci) {
		// Not Freddy: he is the player's friend. And not only what it can see (the host's walls
		// aren't blocks, so sight is unreliable here).
		this.targetSelector.addGoal(3, new NearestAttackableTargetGoal<>(this, SkyrimActorEntity.class, 10, false, false,
			(target, level) -> !target.getName().getString().toLowerCase(java.util.Locale.ROOT).contains("freddy")));
	}
}
