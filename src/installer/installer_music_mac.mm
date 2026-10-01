#include "installer/installer_music_mac.h"

#import <AVFoundation/AVFoundation.h>

namespace eot::installer::mac {

void *StartPlayer(const void *data, size_t size, std::string &error) {
  @autoreleasepool {
    NSData *clip = [NSData dataWithBytesNoCopy:const_cast<void *>(data) length:size freeWhenDone:NO];
    NSError *failure = nil;
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithData:clip error:&failure];
    if (!player) {
      error = failure ? failure.localizedDescription.UTF8String : "no player for the clip";
      return nullptr;
    }
    player.volume = 0.0f;
    if (![player play]) {
      [player release];
      error = "the player would not start";
      return nullptr;
    }
    return player;
  }
}

void SetPlayerVolume(void *player, float volume) {
  if (player)
    ((AVAudioPlayer *)player).volume = volume;
}

void StopPlayer(void *player) {
  if (!player)
    return;
  @autoreleasepool {
    AVAudioPlayer *p = (AVAudioPlayer *)player;
    [p stop];
    [p release];
  }
}

}
