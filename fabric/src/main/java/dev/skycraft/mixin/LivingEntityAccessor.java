package dev.skycraft.mixin;

import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Lets a host character's stand-in tell clients what it wears (its tick skips LivingEntity's). */
@Mixin(LivingEntity.class)
public interface LivingEntityAccessor {
	@Invoker("detectEquipmentUpdates")
	void skycraft$detectEquipmentUpdates();
}
