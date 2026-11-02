#pragma once

#include "pal/linux/renderer/MapScene.h"
#include "contracts/NavigationTypes.h"

namespace p1::renderer {

MapScene makeMockMapScene();
contracts::NavigationSnapshot makeMockNavigationSnapshot();

} // namespace p1::renderer
