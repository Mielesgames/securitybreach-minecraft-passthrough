package dev.skycraft.mixin;

import net.minecraft.world.entity.Pose;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Security Breach: sneaking into a vent crawls (SkyClient decides, SkyCraft.crawling). Minecraft
 * only crawls where it already can't stand or crouch, and its own check doesn't see the game's
 * geometry, so in a vent it would stand up into the ceiling and be pushed out on top.
 */
@Mixin(Player.class)
public abstract class PlayerCrawlMixin {
	@Inject(method = "updatePlayerPose", at = @At("HEAD"), cancellable = true)
	private void skycraft$crawl(CallbackInfo ci) {
		if (dev.skycraft.SkyCraft.crawling && dev.skycraft.world.SkyDig.PASSTHROUGH) {
			((Player) (Object) this).setPose(Pose.SWIMMING);
			ci.cancel();
		}
	}
}
