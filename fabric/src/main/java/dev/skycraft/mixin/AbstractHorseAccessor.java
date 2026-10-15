package dev.skycraft.mixin;

import net.minecraft.world.entity.animal.equine.AbstractHorse;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Lets Freddy's mule get its chest slots without a player putting the chest on. */
@Mixin(AbstractHorse.class)
public interface AbstractHorseAccessor {
	@Invoker("createInventory")
	void skycraft$createInventory();

	@org.spongepowered.asm.mixin.gen.Accessor("inventory")
	net.minecraft.world.SimpleContainer skycraft$inventory();
}
