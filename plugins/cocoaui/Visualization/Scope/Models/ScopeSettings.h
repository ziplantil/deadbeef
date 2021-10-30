//
//  ScopeSettings.h
//  deadbeef
//
//  Created by Alexey Yakovenko on 30/10/2021.
//  Copyright © 2021 Alexey Yakovenko. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "scope.h"

typedef NS_ENUM(NSInteger, ScopeScaleMode) {
    ScopeScaleModeAuto,
    ScopeScaleMode1x,
    ScopeScaleMode2x,
    ScopeScaleMode3x,
    ScopeScaleMode4x,
};

NS_ASSUME_NONNULL_BEGIN

@interface ScopeSettings : NSObject

@property (nonatomic) ddb_scope_mode_t renderMode;
@property (nonatomic) ScopeScaleMode scaleMode;

@end

NS_ASSUME_NONNULL_END
