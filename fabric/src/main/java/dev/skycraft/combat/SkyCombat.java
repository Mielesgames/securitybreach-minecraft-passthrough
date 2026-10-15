package dev.skycraft.combat;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and Skyrim actors, server side.
 *
 * <p>Every Skyrim actor near the player gets an invisible {@link SkyrimActorEntity} at its exact
 * position. Minecraft weapons hit those like any mob; the resulting damage is sent to Skyrim, which
 * applies it to the real actor (scaled by level) and makes it fight back. Skyrim's hits on the player
 * come back as Minecraft damage from the attacker's stand-in, so armor, shields, knockback, hurt
 * sounds and death all work the Minecraft way.
 */
public final class SkyCombat {
	public static final ResourceKey<EntityType<?>> SKYRIM_ACTOR_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "skyrim_actor"));
	public static final EntityType<SkyrimActorEntity> SKYRIM_ACTOR = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		SKYRIM_ACTOR_KEY,
		EntityType.Builder.<SkyrimActorEntity>of(SkyrimActorEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(SKYRIM_ACTOR_KEY)
	);

	/** Skyrim damage is divided by this for Minecraft (a 15-damage bandit swing = 3 = 1.5 hearts). */
	public static final float SKYRIM_TO_MC_DAMAGE = 5.0F;

	private static final Map<Integer, SkyrimActorEntity> PROXIES = new HashMap<>();
	private static final List<SkyLink.Actor> ACTORS = new ArrayList<>();

	private SkyCombat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(SKYRIM_ACTOR, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(SkyCombat::serverTick);
	}

	public static @Nullable SkyrimActorEntity proxy(int formId) {
		return PROXIES.get(formId);
	}

	/** Tag on a mob the mod keeps from falling (kept with the mob, so a saved one gets gravity back). */
	private static final String FLOATING_TAG = "skycraft_floating";

	/**
	 * Security Breach's floors reach Minecraft only around the player. A mob further out (a jockey's
	 * chicken spawned where its animatronic stood) had no floor and fell through it, taking the
	 * animatronic along (15 Oct). Until its spot is known it hangs where it is, like a mob in an
	 * unloaded chunk.
	 */
	private static void holdInUnknown(ServerLevel level) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || !dev.skycraft.world.SkyCollision.active()) {
			return;
		}
		for (var entity : level.getAllEntities()) {
			if (!(entity instanceof net.minecraft.world.entity.Mob mob)) {
				continue;
			}
			boolean held = mob.entityTags().contains(FLOATING_TAG);
			boolean known = dev.skycraft.world.SkyCollision.isKnown(mob.getBoundingBox().expandTowards(0, -1.0, 0));
			if (!known && (held || !mob.isNoGravity())) {
				if (!held) {
					mob.addTag(FLOATING_TAG);
					SkyCraft.LOG.info("FNAFSBCraft: {} waits for the game's floor under it", mob.getName().getString());
				}
				mob.setNoGravity(true);
				var motion = mob.getDeltaMovement();
				mob.setDeltaMovement(0, Math.max(0, motion.y), 0);
			} else if (known && held) {
				mob.removeTag(FLOATING_TAG);
				mob.setNoGravity(false);
			}
		}
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!SkyLink.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (SkyLink.readActors(ACTORS)) {
			sync(level);
		}
		FreddyMount.follow(level, PROXIES.values());
		golemsReach(level);
		survivalOnly(level);
		anvilsFall(level);
		holdInUnknown(level);
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor.
		for (SkyrimActorEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				SkyLink.pushEvent(
					Proto.EV_HIT_ACTOR, proxy.formId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5])
				);
				SkyCraft.LOG.info("FNAFSBCraft: hit {} for {} (knockback {})", proxy.getName().getString(), hit[0], hit[3]);
				// A jockey: the blow throws the chicken, and the character rides along.
				var rider = JOCKEYS.get(proxy.formId());
				var chicken = rider != null ? rider.getVehicle() : null;
				if (chicken != null && hit[3] > 0.0F) {
					float strength = Math.min(hit[3], 3.0F);
					chicken.setDeltaMovement(chicken.getDeltaMovement().add(hit[1] * 0.5 * strength, 0.35 + 0.1 * strength, hit[2] * 0.5 * strength));
					// Tell the clients it moved, like a hit does.
					chicken.needsSync = true;
					SkyCraft.LOG.info("FNAFSBCraft: {}'s chicken is thrown too", proxy.getName().getString());
				}
			}
		}
	}

	private static final Map<Integer, Long> NEXT_ATTACK = new HashMap<>();

	/** A hostile host character (Security Breach's animatronics) hurts villagers it walks into. */
	private static void attackVillagers(ServerLevel level, SkyrimActorEntity proxy) {
		long now = level.getGameTime();
		if (NEXT_ATTACK.getOrDefault(proxy.formId(), 0L) > now) {
			return;
		}
		var victims = level.getEntitiesOfClass(net.minecraft.world.entity.npc.villager.Villager.class, proxy.getBoundingBox().inflate(0.6));
		if (victims.isEmpty()) {
			return;
		}
		NEXT_ATTACK.put(proxy.formId(), now + 20);
		for (var villager : victims) {
			villager.hurtServer(level, level.damageSources().mobAttack(proxy), 4.0F);
		}
	}

	private static final Map<Integer, net.minecraft.world.entity.animal.allay.Allay> LEAD_HOLDERS = new HashMap<>();
	private static final String LEAD_TAG = "skycraft_lead_holder";

	/**
	 * A lead on a host character is Minecraft's own lead, on an allay nobody sees that is kept where
	 * the character's stand-in is. So it is drawn, tied to fences and broken the way leads are; the
	 * host only hears where the other end is and keeps the character within reach of it.
	 */
	public static void leash(net.minecraft.server.level.ServerPlayer player, int formId, boolean on) {
		var held = LEAD_HOLDERS.remove(formId);
		if (held != null) {
			// Off: the lead drops where the character stands, like off any animal.
			if (!held.isRemoved() && held.isLeashed()) {
				held.dropLeash();
			}
			held.discard();
		}
		SkyrimActorEntity proxy = PROXIES.get(formId);
		if (!on || proxy == null || !(player.level() instanceof ServerLevel level)) {
			SkyLink.pushEvent(Proto.EV_LEASH, formId, 0, 0, 0, 0, 0);
			return;
		}
		var holder = net.minecraft.world.entity.EntityTypes.ALLAY.create(level, net.minecraft.world.entity.EntitySpawnReason.COMMAND);
		if (holder == null) {
			return;
		}
		holder.addTag(LEAD_TAG);
		holder.setNoAi(true);
		holder.setNoGravity(true);
		holder.setSilent(true);
		holder.setPermanentlyInvulnerable(true);
		holder.setPersistenceRequired();
		holder.setInvisible(true);
		holder.addEffect(new net.minecraft.world.effect.MobEffectInstance(
			net.minecraft.world.effect.MobEffects.INVISIBILITY, net.minecraft.world.effect.MobEffectInstance.INFINITE_DURATION, 0, false, false));
		holder.snapTo(proxy.getX(), proxy.getY() + proxy.getBbHeight() * 0.5, proxy.getZ(), proxy.getYRot(), 0.0F);
		if (!level.addFreshEntity(holder)) {
			return;
		}
		holder.setLeashedTo(player, true);
		SkyCraft.award(level.getServer(), "leash");
		if (!player.getAbilities().instabuild && player.getMainHandItem().is(net.minecraft.world.item.Items.LEAD)) {
			player.getMainHandItem().shrink(1);
		}
		LEAD_HOLDERS.put(formId, holder);
		SkyCraft.LOG.info("FNAFSBCraft: {} is on a lead", proxy.getName().getString());
	}

	/** Keeps the lead's allay on the stand-in and tells the host where the lead's other end is. */
	private static void followLead(SkyrimActorEntity proxy, int formId) {
		var holder = LEAD_HOLDERS.get(formId);
		if (holder == null) {
			return;
		}
		var end = holder.isRemoved() ? null : holder.getLeashHolder();
		if (end == null) {
			// Snapped, cut, or the knot was punched off: Minecraft dropped the lead already.
			LEAD_HOLDERS.remove(formId);
			holder.discard();
			SkyLink.pushEvent(Proto.EV_LEASH, formId, 0, 0, 0, 0, 0);
			SkyCraft.LOG.info("FNAFSBCraft: {} is off the lead", proxy.getName().getString());
			return;
		}
		holder.snapTo(proxy.getX(), proxy.getY() + proxy.getBbHeight() * 0.5, proxy.getZ(), proxy.getYRot(), 0.0F);
		holder.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
		// A hand is about a block up; a knot sits in the middle of its fence.
		double up = end instanceof net.minecraft.world.entity.player.Player ? 1.0 : 0.5;
		SkyLink.pushEvent(Proto.EV_LEASH, formId, (float) end.getX(), (float) (end.getY() + up), (float) end.getZ(), 0, 1);
	}

	private static final Map<Integer, net.minecraft.world.entity.monster.zombie.Zombie> JOCKEYS = new HashMap<>();
	private static final java.util.Set<Integer> JOCKEY_ROLLED = new java.util.HashSet<>();
	private static final java.util.Set<Integer> JOCKEY_FORCED = new java.util.HashSet<>();
	private static String jockeyName;
	private static long jockeyUntil;

	/**
	 * A command asked for a jockey: the nearest character of that name becomes one, or (summoning)
	 * the one about to appear does.
	 */
	public static void forceJockey(String name, boolean newOne) {
		String wanted = name.toLowerCase(java.util.Locale.ROOT);
		if (newOne) {
			jockeyName = wanted;
			jockeyUntil = System.currentTimeMillis() + 5000;
			return;
		}
		SkyrimActorEntity best = null;
		for (var proxy : PROXIES.values()) {
			if (!proxy.getName().getString().toLowerCase(java.util.Locale.ROOT).contains(wanted)) {
				continue;
			}
			var player = proxy.level().getNearestPlayer(proxy, 256.0);
			if (best == null || (player != null && proxy.distanceToSqr(player) < best.distanceToSqr(player))) {
				best = proxy;
			}
		}
		if (best != null) {
			JOCKEY_FORCED.add(best.formId());
		}
	}

	/**
	 * Where a host character wears this, or null if it doesn't: a carved pumpkin on its head (it can't
	 * see), ice or frost walker boots on its feet (it slips).
	 */
	public static net.minecraft.world.entity.@org.jspecify.annotations.Nullable EquipmentSlot wearable(net.minecraft.world.item.ItemStack stack) {
		if (stack.is(net.minecraft.world.item.Items.CARVED_PUMPKIN)) {
			return net.minecraft.world.entity.EquipmentSlot.HEAD;
		}
		if (sticky(stack)) {
			return net.minecraft.world.entity.EquipmentSlot.LEGS;
		}
		if (stack.is(net.minecraft.world.item.Items.ENDER_PEARL)) {
			return net.minecraft.world.entity.EquipmentSlot.MAINHAND;
		}
		return slippery(stack) ? net.minecraft.world.entity.EquipmentSlot.FEET : null;
	}

	/** Slime or honey on a character: it gets stuck, walking at a third of its pace. */
	private static boolean sticky(net.minecraft.world.item.ItemStack stack) {
		return stack.is(net.minecraft.world.item.Items.SLIME_BALL) || stack.is(net.minecraft.world.item.Items.SLIME_BLOCK)
			|| stack.is(net.minecraft.world.item.Items.HONEY_BLOCK) || stack.is(net.minecraft.world.item.Items.HONEY_BOTTLE)
			|| stack.is(net.minecraft.world.item.Items.HONEYCOMB);
	}

	private static boolean slippery(net.minecraft.world.item.ItemStack stack) {
		if (stack.is(net.minecraft.world.item.Items.ICE) || stack.is(net.minecraft.world.item.Items.PACKED_ICE) || stack.is(net.minecraft.world.item.Items.BLUE_ICE)) {
			return true;
		}
		return stack.is(net.minecraft.tags.ItemTags.FOOT_ARMOR)
			&& stack.getEnchantments().keySet().stream().anyMatch(e -> e.is(net.minecraft.world.item.enchantment.Enchantments.FROST_WALKER));
	}

	/** What the player holds goes on a character (see wearable). */
	public static void wearPumpkin(net.minecraft.server.level.ServerPlayer player, int formId) {
		SkyrimActorEntity proxy = PROXIES.get(formId);
		var held = player.getMainHandItem();
		var slot = wearable(held);
		if (proxy == null || slot == null || !proxy.getItemBySlot(slot).isEmpty()) {
			return;
		}
		proxy.setItemSlot(slot, held.copyWithCount(1));
		if (!player.getAbilities().instabuild) {
			held.shrink(1);
		}
		SkyCraft.LOG.info("FNAFSBCraft: {} wears {} now", proxy.getName().getString(), proxy.getItemBySlot(slot).getHoverName().getString());
	}

	/** Shears on a character: what its stand-in wears comes off and drops. */
	public static void shear(net.minecraft.server.level.ServerPlayer player, int formId) {
		SkyrimActorEntity proxy = PROXIES.get(formId);
		if (proxy == null) {
			return;
		}
		for (var slot : new net.minecraft.world.entity.EquipmentSlot[] { net.minecraft.world.entity.EquipmentSlot.HEAD, net.minecraft.world.entity.EquipmentSlot.FEET, net.minecraft.world.entity.EquipmentSlot.LEGS, net.minecraft.world.entity.EquipmentSlot.MAINHAND, net.minecraft.world.entity.EquipmentSlot.CHEST }) {
			var worn = proxy.getItemBySlot(slot);
			if (!worn.isEmpty()) {
				// Into the hand that cut it: dropped at the character's feet it would put it on again.
				if (player == null || !player.getInventory().add(worn.copy())) {
					proxy.spawnAtLocation((ServerLevel) proxy.level(), worn.copy());
				}
				proxy.setItemSlot(slot, net.minecraft.world.item.ItemStack.EMPTY);
				SkyCraft.LOG.info("FNAFSBCraft: {} was sheared ({})", proxy.getName().getString(), slot.getName());
				return; // one thing a cut: the pumpkin first, then the wings
			}
		}
	}
	private static final String JOCKEY_TAG = "skycraft_jockey";
	/** One hostile character in ten is a chicken jockey. */
	private static final float JOCKEY_CHANCE = 0.1F;

	/**
	 * A chicken jockey: a chicken with a baby zombie nobody sees on it, which rides after the player
	 * the way jockeys do. The host keeps the character on the chicken. Decided once per character,
	 * the first time it comes near; never Freddy. True while it rides.
	 */
	private static boolean jockey(ServerLevel level, SkyrimActorEntity proxy, SkyLink.Actor a) {
		int id = a.formId();
		boolean fresh = JOCKEY_ROLLED.add(id);
		// "/summon chica jockey": the next new one of that name rides, whatever the dice say.
		boolean named = fresh && jockeyName != null && System.currentTimeMillis() < jockeyUntil
			&& a.name().toLowerCase(java.util.Locale.ROOT).contains(jockeyName);
		if (named) {
			jockeyName = null;
		}
		boolean forced = JOCKEY_FORCED.remove(id) || named;
		if (!JOCKEYS.containsKey(id) && (forced || (fresh && a.hostile() && level.getRandom().nextFloat() < JOCKEY_CHANCE))) {
			var chicken = net.minecraft.world.entity.EntityTypes.CHICKEN.create(level, net.minecraft.world.entity.EntitySpawnReason.COMMAND);
			var rider = net.minecraft.world.entity.EntityTypes.ZOMBIE.create(level, net.minecraft.world.entity.EntitySpawnReason.COMMAND);
			if (chicken != null && rider != null) {
				chicken.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				chicken.addTag(JOCKEY_TAG);
				chicken.setPersistenceRequired();
				rider.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				rider.addTag(JOCKEY_TAG);
				rider.setBaby(true);
				rider.setSilent(true);
				rider.setPersistenceRequired();
				rider.setInvisible(true);
				rider.addEffect(new net.minecraft.world.effect.MobEffectInstance(
					net.minecraft.world.effect.MobEffects.INVISIBILITY, net.minecraft.world.effect.MobEffectInstance.INFINITE_DURATION, 0, false, false));
				if (level.addFreshEntity(chicken) && level.addFreshEntity(rider) && rider.startRiding(chicken, true, false)) {
					JOCKEYS.put(id, rider);
					SkyCraft.award(level.getServer(), "jockey");
					SkyCraft.LOG.info("FNAFSBCraft: {} is a chicken jockey", proxy.getName().getString());
				} else {
					chicken.discard();
					rider.discard();
				}
			}
		}
		var rider = JOCKEYS.get(id);
		if (rider == null) {
			return false;
		}
		var chicken = rider.getVehicle();
		if (rider.isRemoved() || !rider.isAlive() || chicken == null) {
			// Killed, or knocked off its chicken: the character walks again.
			JOCKEYS.remove(id);
			if (!rider.isRemoved()) {
				rider.discard();
			}
			SkyCraft.LOG.info("FNAFSBCraft: {} lost its chicken", proxy.getName().getString());
			return false;
		}
		// After the player, also in creative (where Minecraft's monsters let players be): the chicken
		// and its rider are sent to the nearest player once a second.
		if (level.getGameTime() % 20 == 0 && chicken instanceof net.minecraft.world.entity.Mob ride) {
			var player = level.getNearestPlayer(chicken, 48.0);
			if (player != null && !player.isSpectator() && chicken.distanceToSqr(player) > 2.0) {
				ride.getNavigation().moveTo(player, 1.4);
				rider.getNavigation().moveTo(player, 1.4);
			}
		}
		// Sitting on the chicken's back.
		SkyLink.pushEvent(Proto.EV_ACTOR_AT, id, (float) chicken.getX(), (float) chicken.getY() + 0.4F, (float) chicken.getZ(), chicken.getYRot(), 0);
		return true;
	}

	/**
	 * Iron golems circled the host's characters without hitting them: the stand-in's box and the
	 * character's real place differ a little, and the melee goal never got "close enough". A golem
	 * whose target is within reach (2.5 blocks across, 3 up or down) hits it once its arm is ready.
	 */
	private static void golemsReach(ServerLevel level) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || PROXIES.isEmpty() || level.getGameTime() % 10 != 0) {
			return;
		}
		for (var proxy : PROXIES.values()) {
			for (var golem : level.getEntitiesOfClass(net.minecraft.world.entity.animal.golem.IronGolem.class, proxy.getBoundingBox().inflate(3.0),
				g -> g.getTarget() == proxy && g.getAttackAnimationTick() <= 0)) {
				double dx = golem.getX() - proxy.getX(), dz = golem.getZ() - proxy.getZ();
				if (dx * dx + dz * dz < 2.5 * 2.5 && Math.abs(golem.getY() - proxy.getY()) < 3.0) {
					golem.getLookControl().setLookAt(proxy);
					if (golem.doHurtTarget(level, proxy)) {
						SkyCraft.award(level.getServer(), "golem");
					}
				}
			}
		}
	}

	/** Per falling anvil (its entity id), the characters it squashed already: once each a fall. */
	/**
	 * Without the gamerule allow_cheating, Security Breach is played in survival: another
	 * game mode is put back once a second, with a word why.
	 */
	private static void survivalOnly(ServerLevel level) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || level.getGameTime() % 20 != 0
			|| level.getServer().overworld().getGameRules().get(SkyCraft.ALLOW_CHEATING)) {
			return;
		}
		for (var player : level.players()) {
			if (player.gameMode() != net.minecraft.world.level.GameType.SURVIVAL) {
				player.setGameMode(net.minecraft.world.level.GameType.SURVIVAL);
				player.sendSystemMessage(net.minecraft.network.chat.Component.literal(
					"Cheating is disabled, you can switch gamemodes only if you set the gamerule allow_cheating to true").withStyle(net.minecraft.ChatFormatting.RED));
				SkyCraft.LOG.info("FNAFSBCraft: {} put back in survival (allow_cheating is off)", player.getName().getString());
			}
		}
	}

	private static final Map<Integer, java.util.Set<Integer>> ANVIL_HITS = new HashMap<>();
	private static final Map<Integer, Integer> SQUASHES = new HashMap<>();

	/** A falling anvil on a host character squashes it (the host halves its height). */
	private static void anvilsFall(ServerLevel level) {
		if (!dev.skycraft.world.SkyDig.PASSTHROUGH || PROXIES.isEmpty()) {
			return;
		}
		java.util.Set<Integer> falling = new java.util.HashSet<>();
		for (var proxy : PROXIES.values()) {
			for (var anvil : level.getEntitiesOfClass(net.minecraft.world.entity.item.FallingBlockEntity.class, proxy.getBoundingBox().inflate(0.1, 0.4, 0.1),
				f -> f.getBlockState().is(net.minecraft.tags.BlockTags.ANVIL) && f.getDeltaMovement().y < -0.05)) {
				falling.add(anvil.getId());
				// Only from above: its bottom over the top half of the character.
				if (anvil.getY() < proxy.getY() + proxy.getBbHeight() * 0.5) {
					continue;
				}
				if (ANVIL_HITS.computeIfAbsent(anvil.getId(), k -> new java.util.HashSet<>()).add(proxy.formId())) {
					SkyLink.pushEvent(Proto.EV_SQUASH, proxy.formId(), 0, 0, 0, 0, 0);
					SkyCraft.award(level.getServer(), "squash");
					// Three times halved is as flat as the host goes.
					if (SQUASHES.merge(proxy.formId(), 1, Integer::sum) >= 3) {
						SkyCraft.award(level.getServer(), "shattered_all");
					}
					level.playSound(null, proxy.blockPosition(), net.minecraft.sounds.SoundEvents.ANVIL_LAND, net.minecraft.sounds.SoundSource.BLOCKS, 1.0F, 1.0F);
					SkyCraft.LOG.info("FNAFSBCraft: an anvil squashed {}", proxy.getName().getString());
				}
			}
		}
		// Anvils that landed (or are gone) are forgotten.
		ANVIL_HITS.keySet().removeIf(id -> !falling.contains(id) && level.getEntity(id) == null);
	}

	private static final Map<Integer, Long> NEXT_BOUNCE = new HashMap<>();
	private static final Map<Integer, Boolean> WINGS = new HashMap<>();
	private static final Map<Integer, Integer> BLIND = new HashMap<>();
	private static final Map<Integer, net.minecraft.world.entity.animal.allay.Allay> GUIDES = new HashMap<>();
	private static final String GUIDE_TAG = "skycraft_freddy_allay";

	/**
	 * Freddy with wings is an allay: a real one nobody sees, which likes the nearest player and so
	 * follows them the way allays do. The host keeps Freddy where the allay is.
	 */
	private static void guide(ServerLevel level, SkyrimActorEntity proxy, int id) {
		var allay = GUIDES.get(id);
		if (allay == null || allay.isRemoved() || allay.level() != level) {
			// Ones left from an earlier session.
			for (var old : level.getEntitiesOfClass(net.minecraft.world.entity.animal.allay.Allay.class, proxy.getBoundingBox().inflate(128.0), e -> e.entityTags().contains(GUIDE_TAG))) {
				old.discard();
			}
			allay = net.minecraft.world.entity.EntityTypes.ALLAY.create(level, net.minecraft.world.entity.EntitySpawnReason.COMMAND);
			if (allay == null) {
				return;
			}
			allay.addTag(GUIDE_TAG);
			allay.setSilent(true);
			allay.setPermanentlyInvulnerable(true);
			allay.setPersistenceRequired();
			allay.setInvisible(true);
			allay.addEffect(new net.minecraft.world.effect.MobEffectInstance(
				net.minecraft.world.effect.MobEffects.INVISIBILITY, net.minecraft.world.effect.MobEffectInstance.INFINITE_DURATION, 0, false, false));
			allay.snapTo(proxy.getX(), proxy.getY() + 1.0, proxy.getZ(), proxy.getYRot(), 0.0F);
			if (!level.addFreshEntity(allay)) {
				return;
			}
			GUIDES.put(id, allay);
			SkyCraft.LOG.info("FNAFSBCraft: {} follows an allay now", proxy.getName().getString());
		}
		var player = level.getNearestPlayer(allay, 64.0);
		if (player != null) {
			allay.getBrain().setMemory(net.minecraft.world.entity.ai.memory.MemoryModuleType.LIKED_PLAYER, player.getUUID());
		}
		// Freddy hangs under it: his feet two blocks below, so his head is about where the allay flies.
		SkyLink.pushEvent(Proto.EV_ACTOR_AT, id, (float) allay.getX(), (float) allay.getY() - 2.0F, (float) allay.getZ(), allay.getYRot(), 0);
	}

	/**
	 * What Minecraft's world does to a host character, told to the host (which moves it): water
	 * carries it along, a bubble column lifts it or drags it down, slime throws it up, a hostile one
	 * goes after a villager nearby, and an elytra from a dispenser gives it wings.
	 */
	private static void surroundings(ServerLevel level, SkyrimActorEntity proxy, SkyLink.Actor a) {
		int id = a.formId();
		long now = level.getGameTime();
		// An elytra dropped at a character's feet goes on, as from a dispenser.
		if (proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.CHEST).isEmpty()) {
			for (var drop : level.getEntitiesOfClass(net.minecraft.world.entity.item.ItemEntity.class, proxy.getBoundingBox().inflate(0.75),
				d -> d.getItem().is(net.minecraft.world.item.Items.ELYTRA))) {
				proxy.setItemSlot(net.minecraft.world.entity.EquipmentSlot.CHEST, drop.getItem().split(1));
				if (drop.getItem().isEmpty()) {
					drop.discard();
				}
				SkyCraft.LOG.info("FNAFSBCraft: {} picked up an elytra", proxy.getName().getString());
				break;
			}
		}
		// A pumpkin, ice or frost walker boots dropped at its feet: on they go.
		for (var drop : level.getEntitiesOfClass(net.minecraft.world.entity.item.ItemEntity.class, proxy.getBoundingBox().inflate(0.75),
			d -> wearable(d.getItem()) != null)) {
			var slot = wearable(drop.getItem());
			if (!proxy.getItemBySlot(slot).isEmpty()) {
				continue;
			}
			proxy.setItemSlot(slot, drop.getItem().split(1));
			if (drop.getItem().isEmpty()) {
				drop.discard();
			}
			SkyCraft.LOG.info("FNAFSBCraft: {} put on {}", proxy.getName().getString(), proxy.getItemBySlot(slot).getHoverName().getString());
		}
		int effects = (proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.HEAD).is(net.minecraft.world.item.Items.CARVED_PUMPKIN) ? 1 : 0)
			| (slippery(proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.FEET)) ? 2 : 0)
			| (sticky(proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.LEGS)) ? 4 : 0)
			| (proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.MAINHAND).is(net.minecraft.world.item.Items.ENDER_PEARL) ? 8 : 0);
		// Standing on ice (any kind) is as slippery as wearing it.
		if (!proxy.isPassenger() && level.getBlockState(net.minecraft.core.BlockPos.containing(a.x(), a.y() - 0.1, a.z())).is(net.minecraft.tags.BlockTags.ICE)) {
			effects |= 2;
		}
		// On ice it has no grip: a player walking into it shoves it along.
		if ((effects & 2) != 0) {
			for (var player : level.getEntitiesOfClass(net.minecraft.world.entity.player.Player.class, proxy.getBoundingBox().inflate(0.35, 0.0, 0.35),
				p -> !p.isSpectator())) {
				double dx = proxy.getX() - player.getX(), dz = proxy.getZ() - player.getZ();
				double far = Math.sqrt(dx * dx + dz * dz);
				if (far > 1.0E-3) {
					SkyLink.pushEvent(Proto.EV_ACTOR_PUSH, id, (float) (dx / far * 0.35), 0, (float) (dz / far * 0.35), 0, 0);
				}
			}
		}
		Integer hadEffects = BLIND.put(id, effects);
		int newly = effects & ~(hadEffects == null ? 0 : hadEffects);
		if ((newly & 1) != 0) {
			SkyCraft.award(level.getServer(), "blind");
		}
		if ((newly & 2) != 0) {
			SkyCraft.award(level.getServer(), "slippery");
		}
		if ((newly & 4) != 0) {
			SkyCraft.award(level.getServer(), "sticky");
		}
		if ((newly & 8) != 0) {
			SkyCraft.award(level.getServer(), "ender");
		}
		if ((hadEffects == null ? effects != 0 : hadEffects != effects) || (effects != 0 && now % 40 == 0)) {
			SkyLink.pushEvent(Proto.EV_ACTOR_BLIND, id, 0, 0, 0, 0, effects);
		}
		boolean wings = proxy.getItemBySlot(net.minecraft.world.entity.EquipmentSlot.CHEST).is(net.minecraft.world.item.Items.ELYTRA);
		Boolean had = WINGS.put(id, wings);
		if (wings && (had == null || !had)) {
			SkyCraft.award(level.getServer(), a.name().toLowerCase(java.util.Locale.ROOT).contains("freddy") ? "freddy_wings" : "wings");
		}
		if ((had == null ? wings : had != wings) || (wings && now % 40 == 0)) {
			SkyLink.pushEvent(Proto.EV_ACTOR_WINGS, id, 0, 0, 0, 0, wings ? 1 : 0);
			if (had == null || had != wings) {
				SkyCraft.LOG.info("FNAFSBCraft: {} {}", proxy.getName().getString(), wings ? "got wings" : "lost its wings");
			}
		}
		if (wings && a.name().toLowerCase(java.util.Locale.ROOT).contains("freddy")) {
			guide(level, proxy, id);
			return;
		}
		var guide = GUIDES.remove(id);
		if (guide != null) {
			guide.discard();
		}
		if (wings) {
			// In the air the host flies it; the ground's water and slime don't reach it.
			return;
		}
		var feet = proxy.blockPosition();
		var state = level.getBlockState(feet);
		double px = 0, py = 0, pz = 0;
		int flags = 0;
		if (state.is(net.minecraft.world.level.block.Blocks.BUBBLE_COLUMN)) {
			py = state.getValue(net.minecraft.world.level.block.BubbleColumnBlock.DRAG_DOWN) ? -0.2 : 0.35;
		} else {
			var fluid = level.getFluidState(feet);
			if (fluid.is(net.minecraft.tags.FluidTags.WATER)) {
				var flow = fluid.getFlow(level, feet);
				px = flow.x * 0.12;
				py = flow.y * 0.12;
				pz = flow.z * 0.12;
			} else if (level.getBlockState(net.minecraft.core.BlockPos.containing(a.x(), a.y() - 0.1, a.z())).is(net.minecraft.world.level.block.Blocks.HONEY_BLOCK)) {
				// Honey: sticky, it walks at a crawl.
				flags = 4;
			} else if (level.getBlockState(net.minecraft.core.BlockPos.containing(a.x(), a.y() - 0.1, a.z())).is(net.minecraft.world.level.block.Blocks.SLIME_BLOCK)) {
				if (NEXT_BOUNCE.getOrDefault(id, 0L) <= now) {
					NEXT_BOUNCE.put(id, now + 12);
					flags = 1;
				}
			} else if (a.hostile()) {
				// Not carried by anything: a villager within ten blocks is worth walking to.
				var villagers = level.getEntitiesOfClass(net.minecraft.world.entity.npc.villager.Villager.class, proxy.getBoundingBox().inflate(10.0, 3.0, 10.0));
				net.minecraft.world.entity.npc.villager.Villager prey = null;
				for (var villager : villagers) {
					if (prey == null || villager.distanceToSqr(proxy) < prey.distanceToSqr(proxy)) {
						prey = villager;
					}
				}
				if (prey != null) {
					double dx = prey.getX() - a.x(), dz = prey.getZ() - a.z();
					double far = Math.sqrt(dx * dx + dz * dz);
					if (far > 0.8) {
						px = dx / far * 0.16;
						pz = dz / far * 0.16;
						flags = 2;
					}
				}
			}
		}
		if (px != 0 || py != 0 || pz != 0 || flags != 0) {
			SkyLink.pushEvent(Proto.EV_ACTOR_PUSH, id, (float) px, (float) py, (float) pz, 0, flags);
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, SkyLink.Actor> live = new HashMap<>();
		for (SkyLink.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.formId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, SkyrimActorEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, SkyrimActorEntity> e = it.next();
			SkyrimActorEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		// The helpers of characters that are gone go with them.
		GUIDES.entrySet().removeIf(e -> {
			if (!live.containsKey(e.getKey())) {
				e.getValue().discard();
				return true;
			}
			return false;
		});
		LEAD_HOLDERS.entrySet().removeIf(e -> {
			if (!live.containsKey(e.getKey())) {
				if (!e.getValue().isRemoved() && e.getValue().isLeashed()) {
					e.getValue().dropLeash();
				}
				e.getValue().discard();
				return true;
			}
			return false;
		});
		JOCKEYS.entrySet().removeIf(e -> {
			if (!live.containsKey(e.getKey())) {
				var ride = e.getValue().getVehicle();
				if (ride != null) {
					ride.discard();
				}
				e.getValue().discard();
				return true;
			}
			return false;
		});
		int before = PROXIES.size();
		for (SkyLink.Actor a : live.values()) {
			SkyrimActorEntity proxy = PROXIES.get(a.formId());
			if (proxy == null) {
				proxy = new SkyrimActorEntity(SKYRIM_ACTOR, level);
				proxy.setFormId(a.formId());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.formId(), proxy);
				continue;
			}
			proxy.setSize(a.width(), a.height());
			if (proxy.isPassenger()) {
				// Stuck in a boat or minecart: now it is Minecraft that says where the character is.
				SkyLink.pushEvent(Proto.EV_ACTOR_AT, a.formId(), (float) proxy.getX(), (float) proxy.getY(), (float) proxy.getZ(), proxy.getYRot(), 0);
				continue;
			}
			proxy.setPos(a.x(), a.y(), a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			stepOnTriggers(level, proxy);
			// Touching an empty boat or minecart: in it goes, like any mob.
			for (var vehicle : level.getEntitiesOfClass(net.minecraft.world.entity.vehicle.VehicleEntity.class, proxy.getBoundingBox().inflate(0.15), v -> v.getPassengers().isEmpty())) {
				if (proxy.startRiding(vehicle, true, false)) {
					SkyCraft.LOG.info("FNAFSBCraft: {} is stuck in a {}", proxy.getName().getString(), vehicle.getType().toShortString());
					break;
				}
			}
			if (a.hostile()) {
				attackVillagers(level, proxy);
			}
			followLead(proxy, a.formId());
			if (jockey(level, proxy, a)) {
				continue;
			}
			surroundings(level, proxy, a);
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			SkyCraft.LOG.info("FNAFSBCraft: {} Skyrim actors mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * Skyrim's NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, SkyrimActorEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Items and stuck arrows on Skyrim ground rest on its collision voxels, which on steep or rough
	 * terrain can sit a little off from where the player (on Skyrim's exact triangles) stands.
	 * Touch them over a slightly bigger area than vanilla's so walking over them picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * Skyrim hit the player. Runs on the server thread. {@code kind} is a Proto.HURT_* value and
	 * {@code skyrimDamage} is what Skyrim would have taken off the player's health.
	 */
	public static void hurtPlayer(ServerPlayer player, int kind, float skyrimDamage, int attackerFormId, int flags) {
		if (!player.isAlive() || skyrimDamage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		SkyrimActorEntity attacker = PROXIES.get(attackerFormId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same form id as one of the host's
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> sources.generic();
		};
		float damage = skyrimDamage / SKYRIM_TO_MC_DAMAGE;
		float healthBefore = player.getHealth();
		boolean blocking = player.isBlocking();
		boolean hurt = player.hurtServer(level, source, damage);
		trainDefence(player, damage, blocking && player.getHealth() >= healthBefore - 1.0E-3F);
		SkyCraft.LOG.info("FNAFSBCraft: Skyrim hit the player for {} ({} Minecraft): health {} -> {}{}", skyrimDamage, damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_POWER_ATTACK) != 0 && !player.isBlocking()) {
			// Power attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/**
	 * Skyrim skills for taking a hit: Block when the shield caught it, otherwise Light or Heavy
	 * Armor by what the player mostly wears (leather, chainmail, gold, copper and turtle count as
	 * light; iron, diamond and netherite as heavy). Only the host's own Skyrim is told.
	 */
	private static void trainDefence(ServerPlayer player, float damage, boolean blocked) {
		if (!dev.skycraft.net.SkyNet.isHost(player) || damage <= 0.0F) {
			return;
		}
		if (blocked) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, Proto.SKILL_BLOCK, damage, 0.0F, 0.0F, 0.0F, 0);
			return;
		}
		int light = 0, heavy = 0;
		for (var slot : new net.minecraft.world.entity.EquipmentSlot[] { net.minecraft.world.entity.EquipmentSlot.HEAD, net.minecraft.world.entity.EquipmentSlot.CHEST,
			net.minecraft.world.entity.EquipmentSlot.LEGS, net.minecraft.world.entity.EquipmentSlot.FEET }) {
			var stack = player.getItemBySlot(slot);
			if (stack.isEmpty()) {
				continue;
			}
			String path = net.minecraft.core.registries.BuiltInRegistries.ITEM.getKey(stack.getItem()).getPath();
			if (path.startsWith("iron_") || path.startsWith("diamond_") || path.startsWith("netherite_")) {
				heavy++;
			} else {
				light++;
			}
		}
		if (light + heavy > 0) {
			SkyLink.pushEvent(Proto.EV_SKILL_USE, heavy > light ? Proto.SKILL_HEAVY_ARMOR : Proto.SKILL_LIGHT_ARMOR, damage * (light + heavy) / 4.0F, 0.0F, 0.0F,
				0.0F, 0);
		}
	}

	/** Form id of the Skyrim actor behind a damage source, or 0. */
	public static int attackerFormId(DamageSource source) {
		return source.getEntity() instanceof SkyrimActorEntity proxy ? proxy.formId() : 0;
	}
}
