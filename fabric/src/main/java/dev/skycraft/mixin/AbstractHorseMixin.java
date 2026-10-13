package dev.skycraft.mixin;

import dev.skycraft.combat.FreddyMount;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.animal.equine.AbstractHorse;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Freddy's mule seats its rider higher: on Freddy's shoulders, not inside his chest.
 * It also takes an elytra from a dispenser (chest slot), which lets Freddy fly.
 */
@Mixin(AbstractHorse.class)
public abstract class AbstractHorseMixin {
	@Inject(method = "getPassengerAttachmentPoint", at = @At("RETURN"), cancellable = true)
	private void skycraft$freddySeat(Entity passenger, EntityDimensions dimensions, float scale, CallbackInfoReturnable<Vec3> cir) {
		if (FreddyMount.isFreddy((Entity) (Object) this)) {
			cir.setReturnValue(cir.getReturnValue().add(0.0, FreddyMount.EXTRA_SEAT, 0.0));
		}
	}

	@Inject(method = "canUseSlot", at = @At("RETURN"), cancellable = true)
	private void skycraft$freddyWearsWings(EquipmentSlot slot, CallbackInfoReturnable<Boolean> cir) {
		if (slot == EquipmentSlot.CHEST && FreddyMount.isFreddy((Entity) (Object) this)) {
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "canDispenserEquipIntoSlot", at = @At("RETURN"), cancellable = true)
	private void skycraft$freddyTakesWings(EquipmentSlot slot, CallbackInfoReturnable<Boolean> cir) {
		if (slot == EquipmentSlot.CHEST && FreddyMount.isFreddy((Entity) (Object) this)) {
			cir.setReturnValue(true);
		}
	}
}
