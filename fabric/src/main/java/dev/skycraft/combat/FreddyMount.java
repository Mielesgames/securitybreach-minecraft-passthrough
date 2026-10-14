package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import net.minecraft.network.chat.Component;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.animal.equine.Mule;
import org.jspecify.annotations.Nullable;

/**
 * Security Breach: Freddy as a mule. Right-clicking him in the host game puts the player on an
 * invisible Minecraft mule that stands where he stands. Minecraft does the riding (saddle to steer,
 * a chest for storage, the mule's inventory screen); the host keeps Freddy under the rider.
 */
public final class FreddyMount {
	/** What the mule is called: how both sides recognise it (names reach the client, tags don't). */
	public static final String NAME = "Freddy";
	/** How much higher than on a mule the rider sits: Freddy is a lot taller. */
	public static final double EXTRA_SEAT = 0.9;

	private static @Nullable Mule mule;

	private FreddyMount() {
	}

	public static boolean isFreddy(@Nullable Entity entity) {
		return entity instanceof Mule m && m.hasCustomName() && NAME.equals(m.getCustomName().getString());
	}

	/** The player used Freddy in the host game, who stands at (x, y, z). */
	public static void mount(ServerPlayer player, double x, double y, double z) {
		ServerLevel level = player.level();
		if (mule == null || mule.isRemoved() || mule.level() != level) {
			mule = level.getEntitiesOfClass(Mule.class, player.getBoundingBox().inflate(256.0), FreddyMount::isFreddy).stream().findFirst().orElse(null);
		}
		if (mule == null) {
			Mule fresh = net.minecraft.world.entity.EntityTypes.MULE.create(level, EntitySpawnReason.COMMAND);
			if (fresh == null) {
				return;
			}
			fresh.setCustomName(Component.literal(NAME));
			fresh.setCustomNameVisible(false);
			fresh.setSilent(true);
			fresh.setPermanentlyInvulnerable(true);
			fresh.setPersistenceRequired();
			fresh.setTamed(true);
			fresh.snapTo(x, y, z, player.getYRot(), 0.0F);
			if (!level.addFreshEntity(fresh)) {
				return;
			}
			mule = fresh;
			SkyCraft.LOG.info("FNAFSBCraft: Freddy's mule created");
		} else {
			mule.snapTo(x, y, z, player.getYRot(), 0.0F);
		}
		outfit(mule);
		player.startRiding(mule);
	}

	/** Invisible (the host draws Freddy), saddled so it can be steered, and with a chest to keep things in. */
	private static void outfit(Mule mule) {
		// The flag alone doesn't last: a living thing is only invisible while it has the effect.
		if (!mule.hasEffect(net.minecraft.world.effect.MobEffects.INVISIBILITY)) {
			mule.addEffect(new net.minecraft.world.effect.MobEffectInstance(
				net.minecraft.world.effect.MobEffects.INVISIBILITY, net.minecraft.world.effect.MobEffectInstance.INFINITE_DURATION, 0, false, false));
		}
		mule.setInvisible(true);
		if (!mule.hasChest()) {
			mule.setChest(true);
			((dev.skycraft.mixin.AbstractHorseAccessor) mule).skycraft$createInventory();
		}
		if (mule.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.SADDLE).isEmpty()) {
			mule.setItemSlot(net.minecraft.world.entity.EquipmentSlot.SADDLE, new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.SADDLE));
		}
	}

	/** While nobody rides it, the mule stays on Freddy (the host moves him about). */
	public static void follow(ServerLevel level, Iterable<SkyrimActorEntity> proxies) {
		if (mule == null || mule.isRemoved() || mule.level() != level || mule.isVehicle()) {
			return;
		}
		for (SkyrimActorEntity proxy : proxies) {
			if (proxy.hasCustomName() && proxy.getCustomName().getString().startsWith(NAME)) {
				mule.snapTo(proxy.getX(), proxy.getY(), proxy.getZ(), mule.getYRot(), 0.0F);
				mule.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
				return;
			}
		}
	}
}
