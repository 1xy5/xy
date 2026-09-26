#include "UsrAI.h"
#include <QDebug>
#include <QString>
#include <QStringList>
#include <QFile>
#include <QTextStream>
#include <unordered_set>

tagGame tagUsrGame;
ins UsrIns;
// AI 的跨帧运行状态。每一帧都会更新其中的关键对象信息。
AiRuntimeState gAiState;

namespace {

// AI 调试日志：同时输出到调试器和 ai_debug.log 文件。
// 仅用于命令行自动验证时观察 AI 决策，不影响游戏逻辑。
void aiLog(const QString& message)
{
    qInfo().noquote() << message;
    QFile f("ai_debug.log");
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream s(&f);
        s << message << "\n";
    }
}

// “附近资源”的搜索半径，单位是地图区块。
constexpr int NEARBY_RESOURCE_RADIUS = 20;

// 给村民下令后，在多少帧内不再对同一村民重新派单。
// 原因：ins_ret 返回成功后，内核可能还需要若干帧才把村民状态切成 WALKING/WORKING，
// 这段时间内 needsResourceReassign 会误判它空闲。冷却期直接跳过，避免重复下令。
constexpr int FARMER_ORDER_COOLDOWN_FRAMES = 80;

// 开局经济分配目标：4 人浆果、2 人木头、1 人石头、1 人建设者预留。
constexpr int TARGET_BERRY_WORKERS = 4;
constexpr int TARGET_WOOD_WORKERS = 2;
constexpr int TARGET_STONE_WORKERS = 1;

// 新村民分工相关常量
constexpr int TARGET_FARMER_COUNT = 20;   // 陆地村民目标数量上限
constexpr int FARMER_FOOD_COST = 50;
constexpr int CENTER_PRODUCTION_RETRY_FRAMES = 300;
constexpr int TARGET_BERRY_TOTAL = 6;

// 建房相关常量
constexpr int HOUSE_POP_GAP = 2;          // 剩余人口容量 <= 2 时开始考虑建房
constexpr int HOUSE_SITE_OFFSET = 5;      // 房屋距基地外侧的初始偏移格数
constexpr int HOUSE_SEARCH_RADIUS = 6;     // 向外搜索建房位置的最大半径


// 根据一个坐标与地图长度，计算从当前位置朝地图中心应走的方向。
// 返回 1 表示坐标应增大，-1 表示坐标应减小，0 表示已经在中线上。
int directionTowardCenter(int coordinate, int mapSize)
{
    if (coordinate < mapSize / 2) return 1;
    if (coordinate > mapSize / 2) return -1;
    return 0;
}

// 将所有运行状态恢复为默认值，用于开始一局新游戏时清除上一局的数据。
void resetRuntimeState()
{
    gAiState = AiRuntimeState();
}

// 从当前帧的信息中重新查找我方的关键建筑和祭司。
// 每帧重新查找可以避免对象被摧毁后仍然使用已经失效的 SN。
void refreshKeyObjects(const tagInfo& info)
{
    // 先清空上一帧的查找结果，再以当前帧数据为准重新填写。
    gAiState.centerSn = -1;
    gAiState.stockSn = -1;
    gAiState.granarySn = -1;
    gAiState.priestSn = -1;
    gAiState.arrowTowerSns.clear();

    // 遍历我方建筑，按建筑类型保存需要的对象。
    for (const tagBuilding& building : info.buildings) {
        switch (building.Type) {
        case BUILDING_CENTER:
            // 第一个市镇中心作为基地，并用它的坐标判断基地位于地图哪一侧。
            if (gAiState.centerSn < 0) {
                gAiState.centerSn = building.SN;
                gAiState.baseDR = building.BlockDR;
                gAiState.baseUR = building.BlockUR;
            }
            break;
        case BUILDING_STOCK:
            if (gAiState.stockSn < 0) gAiState.stockSn = building.SN;
            break;
        case BUILDING_GRANARY:
            if (gAiState.granarySn < 0) gAiState.granarySn = building.SN;
            break;
        case BUILDING_ARROWTOWER:
            gAiState.arrowTowerSns.push_back(building.SN);
            break;
        default:
            break;
        }
    }

    // 祭司属于军队单位，因此要在 armies 列表中单独查找。
    for (const tagArmy& army : info.armies) {
        if (army.Sort == AT_PRIEST) {
            gAiState.priestSn = army.SN;
            break;
        }
    }

    // 找到基地后，分别计算 DR 和 UR 两个坐标轴上朝地图中心的方向。
    if (gAiState.centerSn >= 0) {
        gAiState.towardCenterDR = directionTowardCenter(gAiState.baseDR, MAP_L);
        gAiState.towardCenterUR = directionTowardCenter(gAiState.baseUR, MAP_U);
    } else {
        gAiState.baseDR = -1;
        gAiState.baseUR = -1;
        gAiState.towardCenterDR = 0;
        gAiState.towardCenterUR = 0;
    }

    // 第一轮把这四个对象全部找到作为初始化完成的条件。
    // 箭塔数量可能变化，因此不把箭塔作为初始化的硬性条件。
    gAiState.initialized =
        gAiState.centerSn >= 0 &&
        gAiState.stockSn >= 0 &&
        gAiState.granarySn >= 0 &&
        gAiState.priestSn >= 0;
}

// 判断资源是否位于基地附近，并且还有可采集内容。
bool isAvailableNearbyResource(const tagResource& resource)
{
    if (resource.Cnt <= 0 && resource.Blood <= 0) return false;

    const int deltaDR = resource.BlockDR - gAiState.baseDR;
    const int deltaUR = resource.BlockUR - gAiState.baseUR;
    return deltaDR * deltaDR + deltaUR * deltaUR <=
           NEARBY_RESOURCE_RADIUS * NEARBY_RESOURCE_RADIUS;
}

// 每帧刷新基地附近的四类基础资源，避免保存已经耗尽或消失的资源点。
void refreshNearbyResources(const tagInfo& info)
{
    gAiState.nearbyBushSns.clear();
    gAiState.nearbyTreeSns.clear();
    gAiState.nearbyStoneSns.clear();
    gAiState.nearbyGoldSns.clear();

    if (gAiState.centerSn < 0) return;

    for (const tagResource& resource : info.resources) {
        if (!isAvailableNearbyResource(resource)) continue;

        switch (resource.Type) {
        case RESOURCE_BUSH:
            gAiState.nearbyBushSns.push_back(resource.SN);
            break;
        case RESOURCE_TREE:
            gAiState.nearbyTreeSns.push_back(resource.SN);
            break;
        case RESOURCE_STONE:
            gAiState.nearbyStoneSns.push_back(resource.SN);
            break;
        case RESOURCE_GOLD:
            gAiState.nearbyGoldSns.push_back(resource.SN);
            break;
        default:
            break;
        }
    }
}

// 初始化完成时只记录当时已有的陆地村民，保证本轮不会管理后来生产的村民。
void captureInitialFarmers(const tagInfo& info)
{
    if (gAiState.initialFarmersCaptured || !gAiState.initialized) return;

    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.FarmerSort == FARMERTYPE_FARMER) {
            gAiState.initialFarmerSns.push_back(farmer.SN);
        }
    }
    gAiState.initialFarmersCaptured = true;

    aiLog(QString("[AI Resources] farmers=%1 bushes=%2 trees=%3 stone=%4 gold=%5")
               .arg(static_cast<int>(gAiState.initialFarmerSns.size()))
               .arg(static_cast<int>(gAiState.nearbyBushSns.size()))
               .arg(static_cast<int>(gAiState.nearbyTreeSns.size()))
               .arg(static_cast<int>(gAiState.nearbyStoneSns.size()))
               .arg(static_cast<int>(gAiState.nearbyGoldSns.size())));
}

// 在当前快照中通过 SN 查找开局村民。
const tagFarmer* findFarmer(const tagInfo& info, int farmerSn)
{
    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.SN == farmerSn) return &farmer;
    }
    return nullptr;
}

// 给一名村民寻找最近的指定类型资源点（浆果/树木/石矿）。
// 优先使用没有被其他村民占用的资源点；allowReserved=true 时允许共享。
int findNearestResource(const tagInfo& info,
                        const tagFarmer& farmer,
                        const std::unordered_set<int>& reservedTargets,
                        int resourceType,
                        bool allowReserved)
{
    int targetSn = -1;
    double bestDistance = 1e18;

    for (const tagResource& resource : info.resources) {
        if (resource.Type != resourceType) continue;
        if (!isAvailableNearbyResource(resource)) continue;
        if (!allowReserved && reservedTargets.count(resource.SN) != 0) continue;

        const double deltaDR = farmer.DR - resource.DR;
        const double deltaUR = farmer.UR - resource.UR;
        const double distance = deltaDR * deltaDR + deltaUR * deltaUR;
        if (distance < bestDistance) {
            bestDistance = distance;
            targetSn = resource.SN;
        }
    }
    return targetSn;
}

// 读取上一帧采集指令的执行结果。失败时解除记录，使该村民可以在后续帧重试。
void processGatherOrderResults(const tagInfo& info)
{
    for (auto it = gAiState.pendingGatherOrders.begin();
         it != gAiState.pendingGatherOrders.end();) {
        const auto result = info.ins_ret.find(it->first);
        if (result == info.ins_ret.end()) {
            ++it;
            continue;
        }

        const int orderId = it->first;
        const int farmerSn = it->second;
        aiLog(QString("[AI Gather Result] order=%1 farmer=%2 result=%3")
                   .arg(orderId)
                   .arg(farmerSn)
                   .arg(result->second));

        if (result->second != ACTION_SUCCESS) {
            // 指令失败：解除该村民的采集目标和工作登记，
            // 让它在下一帧被当作"未分配"重新尝试，而不是卡在旧记录上。
            gAiState.farmerGatherTargets.erase(farmerSn);
            gAiState.farmerJobs.erase(farmerSn);
        }
        it = gAiState.pendingGatherOrders.erase(it);
    }
}

// 读取市镇中心造村民指令的执行结果。
// 成功后清除待确认标记；失败后记录帧号，等待间隔后再试。
void processCenterProductionResult(const tagInfo& info)
{
    if (gAiState.pendingCenterProductionOrder < 0) return;
    const auto result = info.ins_ret.find(gAiState.pendingCenterProductionOrder);
    if (result == info.ins_ret.end()) return; // 结果还没回来

    aiLog(QString("[AI Center Produce Result] order=%1 result=%2")
              .arg(gAiState.pendingCenterProductionOrder)
              .arg(result->second));

    // 无论成功失败都清除待确认标记，允许下一帧重新判断。
    gAiState.pendingCenterProductionOrder = -1;
    gAiState.lastCenterProductionFrame = gAiState.lastFrame;
}

// 市镇中心持续生产村民：空闲、人口未满、食物足够时下令造村民。
// 同一条指令未返回结果前不重复下令；失败后等待 CENTER_PRODUCTION_RETRY_FRAMES 再试。
void manageCenterProduction(UsrAI& ai, const tagInfo& info)
{
    if (gAiState.centerSn < 0) return;
    if (gAiState.pendingCenterProductionOrder >= 0) return; // 上一条指令还没结算

    // 统计陆地村民数量（排除渔船、运输船等）。
    int landFarmers = 0;
    for (const tagFarmer& f : info.farmers) {
        if (f.FarmerSort == FARMERTYPE_FARMER) ++landFarmers;
    }

    if (gAiState.lastFrame % 500 == 0) {
        aiLog(QString("[AI Population] frame=%1 farmers=%2 totalPopulation=%3 maxPopulation=%4")
                  .arg(gAiState.lastFrame).arg(landFarmers)
                  .arg(static_cast<int>(info.Human_Num)).arg(info.Human_MaxNum));
    }

    if (info.Human_Num >= info.Human_MaxNum) return;
    if (landFarmers >= TARGET_FARMER_COUNT) {
        if (!gAiState.productionStoppedLogged) {
            aiLog(QString("[AI Farmer Production Stop] frame=%1 farmers=%2")
                      .arg(gAiState.lastFrame).arg(landFarmers));
            gAiState.productionStoppedLogged = true;
        }
        return;
    }

    // 食物不足时等下一帧。
    if (info.Meat < FARMER_FOOD_COST) return;

    // 失败后间隔重试。
    if (gAiState.lastCenterProductionFrame >= 0 &&
        gAiState.lastFrame - gAiState.lastCenterProductionFrame < CENTER_PRODUCTION_RETRY_FRAMES) {
        return;
    }

    // 检查市镇中心是否空闲（Project == ACT_NULL 表示没有正在生产/升级）。
    for (const tagBuilding& building : info.buildings) {
        if (building.SN != gAiState.centerSn) continue;
        if (building.Project != ACT_NULL) return; // 正在生产村民或升级时代
        break;
    }

    const int orderId = ai.BuildingAction(gAiState.centerSn, BUILDING_CENTER_CREATEFARMER);
    gAiState.pendingCenterProductionOrder = orderId;
    aiLog(QString("[AI Center Produce Farmer] order=%1 center=%2 meat=%3 pop=%4/%5")
              .arg(orderId).arg(gAiState.centerSn).arg(info.Meat)
              .arg(static_cast<int>(info.Human_Num)).arg(info.Human_MaxNum));
}

// 扫描当前快照，把新生产出来的村民登记到 farmerJobs（标记为未分配）。
// 开局村民由 captureInitialFarmers + 原 4/2/1/1 逻辑处理，这里只登记新村民。
// 每帧只做登记（不下令），具体分工由 manageFarmerJobs 完成。
void registerNewFarmers(const tagInfo& info)
{
    if (!gAiState.initialFarmersCaptured) return;

    std::unordered_set<int> initialSet(gAiState.initialFarmerSns.begin(),
                                        gAiState.initialFarmerSns.end());

    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.FarmerSort != FARMERTYPE_FARMER) continue;
        if (gAiState.farmerJobs.count(farmer.SN) != 0) continue;      // 已登记
        if (initialSet.count(farmer.SN) != 0) continue;               // 开局村民由原逻辑处理

        gAiState.farmerJobs[farmer.SN] = FARMER_JOB_UNASSIGNED;
        aiLog(QString("[AI New Farmer] farmer=%1 totalRegistered=%2")
                  .arg(farmer.SN)
                  .arg(static_cast<int>(gAiState.farmerJobs.size())));
    }
}

// 确保有一名固定建设者。builderSn 一旦选定就固定，只有死亡或类型变化才换人。
void ensureBuilder(const tagInfo& info)
{
    // 当前建设者仍在场上且是村民类型则无需换人。
    if (gAiState.builderSn >= 0) {
        bool alive = false;
        bool isFarmer = false;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.SN == gAiState.builderSn) {
                alive = true;
                isFarmer = (farmer.FarmerSort == FARMERTYPE_FARMER);
                break;
            }
        }
        if (alive && isFarmer) return;
        // 原建设者死亡或类型变化：记录旧 SN，从伐木村民中重新选一人。
        aiLog(QString("[AI Builder Replaced] old=%1 reason=%2")
                  .arg(gAiState.builderSn).arg(alive ? "type_changed" : "died"));
        gAiState.builderSn = -1;
        gAiState.buildSiteSn = -1;
        gAiState.builderNeedsWood = false;
    }

    // 从 farmerJobs 中找一名伐木村民改作建设者；找不到再找采石头的。
    int chosen = -1;
    for (const auto& entry : gAiState.farmerJobs) {
        if (entry.second == FARMER_JOB_WOOD) { chosen = entry.first; break; }
    }
    if (chosen < 0) {
        for (const auto& entry : gAiState.farmerJobs) {
            if (entry.second == FARMER_JOB_STONE) { chosen = entry.first; break; }
        }
    }
    if (chosen < 0) return; // 暂时没有可抽调的村民，等下一帧。

    gAiState.builderSn = chosen;
    gAiState.farmerJobs[chosen] = FARMER_JOB_BUILDER;
    // 清除该村民原来的采集目标，避免采集管理再派它去采集。
    gAiState.farmerGatherTargets.erase(chosen);
    aiLog(QString("[AI Builder Fixed] builder=%1").arg(chosen));
    // 输出建设者当前状态诊断。
    const tagFarmer* builder = findFarmer(info, chosen);
    if (builder != nullptr) {
        aiLog(QString("[AI Builder] builder=%1 state=%2 workObj=%3 resSort=%4 res=%5 pos=(%6,%7)")
                  .arg(chosen).arg(builder->NowState).arg(builder->WorkObjectSN)
                  .arg(builder->ResourceSort).arg(builder->Resource)
                  .arg(builder->DR, 0, 'f', 1).arg(builder->UR, 0, 'f', 1));
    } else {
        aiLog(QString("[AI Builder] builder=%1 not found in farmers!").arg(chosen));
    }
}

// 检查候选块坐标是否可建房：地图边界、不与现有建筑重叠、不在失败列表。
bool isBuildSiteValid(const tagInfo& info, int blockDR, int blockUR)
{
    // 地图边界检查。
    if (blockDR < 0 || blockUR < 0) return false;
    if (info.theMap == nullptr) return false;
    const size_t mapSize = info.theMap->size();
    if (static_cast<size_t>(blockDR) >= mapSize) return false;
    if (static_cast<size_t>(blockUR) >= (*info.theMap)[blockDR].size()) return false;

    // 建筑重叠检查：与现有建筑保持至少 1 格距离。
    for (const tagBuilding& building : info.buildings) {
        if (abs(building.BlockDR - blockDR) <= 1 && abs(building.BlockUR - blockUR) <= 1) {
            return false;
        }
    }


    // 失败过的位置不再重复尝试。
    const int encoded = blockDR * 1000 + blockUR;
    if (gAiState.failedBuildPositions.count(encoded) != 0) return false;

    return true;
}

// 在基地周围全方向搜索合法建房位置。从近到远做同心圆扫描，找到第一个合法位置。
bool findBuildSite(const tagInfo& info, int& outDR, int& outUR)
{
    if (gAiState.baseDR < 0 || gAiState.baseUR < 0) return false;

    // 从基地向外做同心圆搜索，r 从 3 到 20 格。
    // 先搜近的环，再搜远的环，找到第一个合法位置就返回。
    for (int r = 3; r <= 30; ++r) {
        // 遍历环边上的所有点。
        for (int d = -r; d <= r; ++d) {
            for (int e = -r; e <= r; ++e) {
                if (abs(d) != r && abs(e) != r) continue; // 只搜环边
                int blockDR = gAiState.baseDR + d;
                int blockUR = gAiState.baseUR + e;
                if (isBuildSiteValid(info, blockDR, blockUR)) {
                    outDR = blockDR;
                    outUR = blockUR;
                    return true;
                }
            }
        }
    }
    return false;
}

// 检查建房指令结果。成功后在建筑列表中找到新建房屋并记录 buildSiteSn；失败则记录位置。
void processBuildOrderResult(const tagInfo& info)
{
    if (gAiState.pendingBuildOrder < 0) return;
    const auto result = info.ins_ret.find(gAiState.pendingBuildOrder);
    if (result == info.ins_ret.end()) return;

    aiLog(QString("[AI House Result] order=%1 result=%2")
              .arg(gAiState.pendingBuildOrder).arg(result->second));

    const int orderId = gAiState.pendingBuildOrder;
    gAiState.pendingBuildOrder = -1;

    if (result->second != ACTION_SUCCESS) {
        // 失败：该位置被标记为失败，后续不再尝试。
        aiLog(QString("[AI Build] order=%1 failed, position marked bad").arg(orderId));
        return;
    }

    // 成功：在建筑列表中找一个新出现的、未完工的房屋。
    for (const tagBuilding& building : info.buildings) {
        if (building.Type != BUILDING_HOME) continue;
        if (building.Percent >= 100) continue;
        gAiState.buildSiteSn = building.SN;
        aiLog(QString("[AI House Progress] sn=%1 started at (%2,%3)")
                  .arg(building.SN).arg(building.BlockDR).arg(building.BlockUR));
        break;
    }
}

// 检查正在建造的房屋进度。建成后让建设者回去伐木。
void checkBuildSiteProgress(const tagInfo& info)
{
    if (gAiState.buildSiteSn < 0) return;

    bool found = false;
    for (const tagBuilding& building : info.buildings) {
        if (building.SN != gAiState.buildSiteSn) continue;
        found = true;
        // 每 200 帧输出一次建造进度。
        if (gAiState.lastFrame % 200 == 0) {
            aiLog(QString("[AI House Progress] sn=%1 percent=%2").arg(building.SN).arg(building.Percent));
        }
        if (building.Percent >= 100) {
            aiLog(QString("[AI House Progress] sn=%1 completed, builder keeps role=%2").arg(gAiState.buildSiteSn).arg(gAiState.builderSn));
            gAiState.buildSiteSn = -1;
            // 保留 builderSn 和 FARMER_JOB_BUILDER 身份；下一帧由 manageBuilding 派他去临时伐木。
            gAiState.builderNeedsWood = (gAiState.builderSn >= 0);
        }
        break;
    }

    // 建筑消失（被摧毁或列表刷新）则清除记录。
    if (!found) {
        gAiState.buildSiteSn = -1;
    }
}

// 总管建房：人口紧张时找位置并下令建造。
void manageBuilding(UsrAI& ai, const tagInfo& info)
{
    checkBuildSiteProgress(info);
    processBuildOrderResult(info);

    if (gAiState.pendingBuildOrder >= 0) return;   // 上一条建房指令未结算
    if (gAiState.buildSiteSn >= 0) return;          // 正在建造房屋，等完工

    // 人口容量充足时不建房，也不选建设者——平时所有人都去采集。
    const int freePop = info.Human_MaxNum - static_cast<int>(info.Human_Num);
    // 统计陆地村民数，村民达标后不再为扩充村民建房。
    int landFarmers = 0;
    bool needHouse = freePop <= HOUSE_POP_GAP;
    if (landFarmers >= TARGET_FARMER_COUNT && info.Human_MaxNum >= landFarmers + HOUSE_POP_GAP + 1) needHouse = false;
    if (needHouse && gAiState.lastFrame % 500 == 0) {
        aiLog(QString("[AI House Need] frame=%1 reason=freePop=%2 farmers=%3/%4").arg(gAiState.lastFrame).arg(freePop).arg(landFarmers).arg(TARGET_FARMER_COUNT));
    }

    // 每 500 帧输出一次人口检查日志，避免刷屏。
    if (gAiState.lastFrame % 500 == 0) {
        aiLog(QString("[AI House Check] population=%1/%2 freePop=%3 needHouse=%4 builderSn=%5")
                  .arg(static_cast<int>(info.Human_Num)).arg(info.Human_MaxNum)
                  .arg(freePop).arg(needHouse ? 1 : 0).arg(gAiState.builderSn));
    }

    // 人口充足：如果固定建设者刚完成房屋，派他去临时伐木。
    if (!needHouse) {
        if (gAiState.builderSn >= 0 && gAiState.builderNeedsWood) {
            const tagFarmer* builder = findFarmer(info, gAiState.builderSn);
            if (builder != nullptr) {
                std::unordered_set<int> reserved;
                const int woodSn = findNearestResource(info, *builder, reserved, RESOURCE_TREE, false);
                if (woodSn >= 0) {
                    const int orderId = ai.HumanAction(gAiState.builderSn, woodSn);
                    gAiState.pendingGatherOrders[orderId] = gAiState.builderSn;
                    gAiState.farmerGatherTargets[gAiState.builderSn] = woodSn;
                    gAiState.builderNeedsWood = false;
                    aiLog(QString("[AI Builder Temporary Wood] builder=%1 target=%2 order=%3")
                              .arg(gAiState.builderSn).arg(woodSn).arg(orderId));
                }
            }
        }
        return;
    }

    // 需要建房：ensureBuilder 只在 builderSn 无效时才换人。
    ensureBuilder(info);
    if (gAiState.builderSn < 0) {
        aiLog(QString("[AI House] need house but no builder available"));
        return;
    }

    // 如果建设者刚被派去采集（有待处理指令），等下一帧再建房，避免指令冲突。
    for (const auto& entry : gAiState.pendingGatherOrders) {
        if (entry.second == gAiState.builderSn) {
            return; // 等下一帧
        }
    }

    aiLog(QString("[AI Builder Resume Build] builder=%1").arg(gAiState.builderSn));

    // 找合法位置。
    int blockDR = -1, blockUR = -1;
    if (!findBuildSite(info, blockDR, blockUR)) {
        // 找不到位置：保留失败列表不清空，等下一帧再试，避免反复选同一位置。
        // 失败列表会在房屋建成后由 checkBuildSiteProgress 自然失效（建筑出现后重叠检查会跳过）。
        if (gAiState.lastFrame % 500 == 0) {
            aiLog(QString("[AI House] no valid site found, failedListSize=%1")
                      .arg(static_cast<int>(gAiState.failedBuildPositions.size())));
        }
        return;
    }

    const int orderId = ai.HumanBuild(gAiState.builderSn, BUILDING_HOME, blockDR, blockUR);
    gAiState.pendingBuildOrder = orderId;
    gAiState.lastBuildFrame = gAiState.lastFrame;
    gAiState.lastBuildBlockDR = blockDR;
    gAiState.lastBuildBlockUR = blockUR;
    aiLog(QString("[AI House Order] order=%1 builder=%2 position=(%3,%4) freePop=%5")
              .arg(orderId).arg(gAiState.builderSn).arg(blockDR).arg(blockUR).arg(freePop));
}

// 在当前快照中判断某个资源 SN 是否仍然存在且还有可采集量。
// 用于发现村民原来的浆果点是否已被采空或消失。
bool isResourceAlive(const tagInfo& info, int resourceSn)
{
    if (resourceSn < 0) return false;
    for (const tagResource& resource : info.resources) {
        if (resource.SN != resourceSn) continue;
        return resource.Cnt > 0 || resource.Blood > 0;
    }
    return false;
}

// 清理已经死亡或从快照消失的村民记录，避免长期持有失效 SN。
// 游戏内村民死亡后不会从 farmerJobs/farmerGatherTargets 自动消失，需要主动清理。
void cleanupDeadFarmerRecords(const tagInfo& info)
{
    std::unordered_set<int> aliveFarmerSns;
    for (const tagFarmer& farmer : info.farmers) {
        aliveFarmerSns.insert(farmer.SN);
    }
    for (auto it = gAiState.farmerJobs.begin(); it != gAiState.farmerJobs.end();) {
        if (aliveFarmerSns.count(it->first) == 0) it = gAiState.farmerJobs.erase(it);
        else ++it;
    }
    for (auto it = gAiState.farmerGatherTargets.begin();
         it != gAiState.farmerGatherTargets.end();) {
        if (aliveFarmerSns.count(it->first) == 0) it = gAiState.farmerGatherTargets.erase(it);
        else ++it;
    }
}

// 汇总当前正在被占用的资源点：
// 1) 所有村民 WorkObjectSN（正在工作/移动的目标）；
// 2) 已登记分配目标。
// 选择新浆果点时优先排除这些点，避免多名村民挤在同一位置。
std::unordered_set<int> collectReservedTargets(const tagInfo& info)
{
    std::unordered_set<int> reservedTargets;
    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.WorkObjectSN >= 0) reservedTargets.insert(farmer.WorkObjectSN);
    }
    for (const auto& assignment : gAiState.farmerGatherTargets) {
        reservedTargets.insert(assignment.second);
    }
    return reservedTargets;
}

// 判断一名已登记为采集资源（浆果/木头/石头）的村民是否需要重新寻找资源点。
// 不需要重派的情况一律跳过，避免对正在正常工作的村民重复下令：
// - 村民仍有未结算指令（pendingGatherOrders 中），等待 ins_ret 结果；
// - 村民正在工作或移动中（包括去上交资源）；
// - 原资源点还在（村民交完货 IDLE 时内核会自动让它回去继续采，AI 不干预）。
bool needsResourceReassign(const tagInfo& info,
                            const tagFarmer& farmer,
                            int oldTargetSn,
                            bool hasPendingOrder)
{
    if (hasPendingOrder) return false;
    if (farmer.NowState == HUMAN_STATE_WORKING ||
        farmer.NowState == HUMAN_STATE_WALKING) return false;

    // 原资源点已消失或采空：必须重新寻找。
    if (!isResourceAlive(info, oldTargetSn)) return true;

    // 原资源点还在：村民可能刚交完货正在准备回去继续采，AI 不干预。
    return false;
}


// 根据工作类型返回对应的资源类型常量（RESOURCE_BUSH/TREE/STONE）。
int resourceTypeForJob(int job)
{
    if (job == FARMER_JOB_BERRY) return RESOURCE_BUSH;
    if (job == FARMER_JOB_WOOD) return RESOURCE_TREE;
    if (job == FARMER_JOB_STONE) return RESOURCE_STONE;
    return -1;
}

// 根据工作类型返回日志中使用的资源名。
const char* resourceNameForJob(int job)
{
    if (job == FARMER_JOB_BERRY) return "bush";
    if (job == FARMER_JOB_WOOD) return "tree";
    if (job == FARMER_JOB_STONE) return "stone";
    return "none";
}

// 祭司分阶段探路状态机：
// 0=向中心方向分段探路，到达25格后横向扫图，直到5000帧
// 1=返程回箭塔
// 2=在箭塔旁防守，等待第一波敌军出现并结束
// 3=第一波结束后继续向中心探路
void managePriest(UsrAI& ai, const tagInfo& info)
{
    if (gAiState.priestSn < 0) return;

    const tagArmy* priest = nullptr;
    for (const tagArmy& army : info.armies) {
        if (army.SN == gAiState.priestSn) { priest = &army; break; }
    }
    if (priest == nullptr) return;

    if (gAiState.priestMaxBlood < 0) gAiState.priestMaxBlood = priest->MaxBlood;
    const bool hurt = priest->Blood < priest->MaxBlood;
    const bool enemyNearby = !info.enemy_armies.empty();
    const bool enemyBuildingNearby = !info.enemy_buildings.empty();

    if (gAiState.pendingPriestOrder >= 0) {
        auto it = info.ins_ret.find(gAiState.pendingPriestOrder);
        if (it != info.ins_ret.end()) {
            aiLog(QString("[AI Priest] order=%1 result=%2 frame=%3 state=%4")
                      .arg(gAiState.pendingPriestOrder).arg(it->second)
                      .arg(gAiState.lastFrame).arg(gAiState.priestState));
            gAiState.pendingPriestOrder = -1;
        }
        return;
    }

    int towerDR = gAiState.baseDR, towerUR = gAiState.baseUR;
    for (const tagBuilding& b : info.buildings) {
        if (b.Type == BUILDING_ARROWTOWER) { towerDR = b.BlockDR; towerUR = b.BlockUR; break; }
    }

    const int curBlockDR = int(priest->DR / BLOCKSIDELENGTH);
    const int curBlockUR = int(priest->UR / BLOCKSIDELENGTH);
    const int distFromBase = abs(curBlockDR - gAiState.baseDR) + abs(curBlockUR - gAiState.baseUR);
    const bool danger = enemyNearby || enemyBuildingNearby || hurt;

    switch (gAiState.priestState) {
    case 0: {
        if (gAiState.lastFrame >= 5000) {
            aiLog(QString("[AI Priest Return] frame=%1 reason=wave1_deadline").arg(gAiState.lastFrame));
            gAiState.lastPriestMoveFrame = -1; gAiState.lastDistanceToTarget = -1.0;
            gAiState.priestState = 1;
            break;
        }
        if (danger) {
            aiLog(QString("[AI Priest] state 0->1 frame=%1 reason=%2")
                      .arg(gAiState.lastFrame)
                      .arg(enemyNearby ? "enemy" : enemyBuildingNearby ? "enemy_building" : "hurt"));
            gAiState.priestState = 1;
            break;
        }
        if (!gAiState.atMaxRange && distFromBase >= 25) {
            gAiState.atMaxRange = true;
            gAiState.lateralSide = -1;
            aiLog(QString("[AI Priest Max Range] frame=%1 beginLateralExplore=true").arg(gAiState.lastFrame));
        }
        if (gAiState.lastPriestMoveFrame >= 0 &&
            gAiState.lastFrame - gAiState.lastPriestMoveFrame < 400) break;

        int targetDR = 0, targetUR = 0;
        bool found = false;
        if (!gAiState.atMaxRange) {
            gAiState.priestExploreStep++;
            targetDR = gAiState.baseDR + gAiState.towardCenterDR * 5 * gAiState.priestExploreStep;
            targetUR = gAiState.baseUR + gAiState.towardCenterUR * 5 * gAiState.priestExploreStep;
            found = true;
        } else {
            for (int attempt = 0; attempt < 8 && !found; ++attempt) {
                const int offD = (attempt % 3 - 1) * 3;
                const int offE = ((attempt / 3) % 3 - 1) * 3;
                targetDR = curBlockDR + offD;
                targetUR = curBlockUR + offE;
                const int encoded = targetDR * 100 + targetUR;
                if (targetDR >= 0 && targetUR >= 0 &&
                    gAiState.visitedSites.count(encoded) == 0) {
                    found = true;
                }
            }
        }
        if (!found) {
            gAiState.lastPriestMoveFrame = gAiState.lastFrame;
            break;
        }
        gAiState.visitedSites.insert(targetDR * 100 + targetUR);
        const double detailDR = (targetDR + 0.5) * double(BLOCKSIDELENGTH);
        const double detailUR = (targetUR + 0.5) * double(BLOCKSIDELENGTH);
        const int orderId = ai.HumanMove(priest->SN, detailDR, detailUR);
        gAiState.pendingPriestOrder = orderId;
        gAiState.lastPriestMoveFrame = gAiState.lastFrame;
        aiLog(QString("[AI Priest] EXPLORE block=(%1,%2) order=%3 distFromBase=%4")
                  .arg(targetDR).arg(targetUR).arg(orderId).arg(distFromBase));
        break;
    }
    case 1: {
        if (abs(curBlockDR - towerDR) <= 2 && abs(curBlockUR - towerUR) <= 2) {
            aiLog(QString("[AI Priest] state 1->2 frame=%1 arrived at tower").arg(gAiState.lastFrame));
            gAiState.priestState = 2;
            gAiState.lastDistanceToTarget = -1.0;
            break;
        }
        const double targetDetailDR = (towerDR + 0.5) * double(BLOCKSIDELENGTH);
        const double targetDetailUR = (towerUR + 0.5) * double(BLOCKSIDELENGTH);
        const double dx = priest->DR - targetDetailDR;
        const double dy = priest->UR - targetDetailUR;
        const double dist = sqrt(dx * dx + dy * dy);

        if (gAiState.lastPriestMoveFrame < 0) {
            const int orderId = ai.HumanMove(priest->SN, targetDetailDR, targetDetailUR);
            gAiState.pendingPriestOrder = orderId;
            gAiState.lastPriestMoveFrame = gAiState.lastFrame;
            gAiState.lastDistanceToTarget = dist;
            gAiState.lastDistanceCheckFrame = gAiState.lastFrame;
            aiLog(QString("[AI Priest] RETURN to tower=(%1,%2) order=%3").arg(towerDR).arg(towerUR).arg(orderId));
            break;
        }
        if (gAiState.lastFrame - gAiState.lastDistanceCheckFrame >= 300) {
            const double improvement = gAiState.lastDistanceToTarget - dist;
            if (improvement < 5.0) {
                aiLog(QString("[AI Priest Move Retry] frame=%1 reason=no_progress oldDistance=%2 newDistance=%3")
                          .arg(gAiState.lastFrame).arg(gAiState.lastDistanceToTarget, 0, 'f', 1).arg(dist, 0, 'f', 1));
                const int orderId = ai.HumanMove(priest->SN, targetDetailDR, targetDetailUR);
                gAiState.pendingPriestOrder = orderId;
                gAiState.lastPriestMoveFrame = gAiState.lastFrame;
            }
            gAiState.lastDistanceToTarget = dist;
            gAiState.lastDistanceCheckFrame = gAiState.lastFrame;
        }
        break;
    }
    case 2: {
        if (gAiState.lastFrame < 6000) break;
        if (enemyNearby) {
            if (!gAiState.waveSeen) {
                aiLog(QString("[AI Wave1 Seen] frame=%1").arg(gAiState.lastFrame));
                gAiState.waveSeen = true;
            }
            gAiState.noEnemyFrames = 0;
        } else if (gAiState.waveSeen) {
            gAiState.noEnemyFrames++;
            if (gAiState.noEnemyFrames == 300) {
                aiLog(QString("[AI Wave1 Clear] frame=%1 safeFrames=%2")
                          .arg(gAiState.lastFrame).arg(gAiState.noEnemyFrames));
                aiLog(QString("[AI Priest Resume Scout] frame=%1").arg(gAiState.lastFrame));
                gAiState.priestState = 3;
                gAiState.priestExploreStep = 5;
                gAiState.lastPriestMoveFrame = -1;
                gAiState.lastDistanceToTarget = -1.0;
            }
        }
        break;
    }
    case 3: {
        if (danger) {
            aiLog(QString("[AI Priest] state 3->1 frame=%1 reason=%2")
                      .arg(gAiState.lastFrame).arg(enemyNearby ? "enemy" : "hurt"));
            gAiState.priestState = 1;
            break;
        }
        if (gAiState.lastPriestMoveFrame >= 0 &&
            gAiState.lastFrame - gAiState.lastPriestMoveFrame < 400) break;
        gAiState.priestExploreStep++;
        int targetDR = gAiState.baseDR + gAiState.towardCenterDR * 5 * gAiState.priestExploreStep;
        int targetUR = gAiState.baseUR + gAiState.towardCenterUR * 5 * gAiState.priestExploreStep;
        int attempts = 0;
        while (gAiState.visitedSites.count(targetDR * 100 + targetUR) != 0 && attempts < 10) {
            gAiState.priestExploreStep++;
            targetDR = gAiState.baseDR + gAiState.towardCenterDR * 5 * gAiState.priestExploreStep;
            targetUR = gAiState.baseUR + gAiState.towardCenterUR * 5 * gAiState.priestExploreStep;
            attempts++;
        }
        gAiState.visitedSites.insert(targetDR * 100 + targetUR);
        const double detailDR = (targetDR + 0.5) * double(BLOCKSIDELENGTH);
        const double detailUR = (targetUR + 0.5) * double(BLOCKSIDELENGTH);
        const int orderId = ai.HumanMove(priest->SN, detailDR, detailUR);
        gAiState.pendingPriestOrder = orderId;
        gAiState.lastPriestMoveFrame = gAiState.lastFrame;
        aiLog(QString("[AI Priest] REEXPLORE block=(%1,%2) order=%3").arg(targetDR).arg(targetUR).arg(orderId));
        break;
    }
    default: break;
    }
}

// 持续村民管理：每帧最多给一名村民下一道采集指令。
// 流程：
// 1) 清理死亡村民的过期记录；
// 2) 给尚未登记工作的开局村民按 4 浆果/2 木头/1 石头/1 建设者首次分配；
// 3) 给原资源点耗尽的采集村民重新寻找同类型资源点（优先无人点，不足时共享）。
// 任何一个分支下了指令后立即 return，保证每帧只安排一名村民。
void manageFarmerJobs(UsrAI& ai, const tagInfo& info)
{
    if (!gAiState.initialFarmersCaptured) return;

    cleanupDeadFarmerRecords(info);

    // 汇总当前正在路上的指令村民，避免刚下令还没结算就重复派单。
    std::unordered_set<int> pendingFarmers;
    for (const auto& entry : gAiState.pendingGatherOrders) {
        pendingFarmers.insert(entry.second);
    }

    const std::unordered_set<int> reservedTargets = collectReservedTargets(info);

    // 统计当前各职业已分配人数（遍历所有登记村民，包括新村民）。
    int berryCount = 0, woodCount = 0, stoneCount = 0;
    for (const auto& entry : gAiState.farmerJobs) {
        if (entry.second == FARMER_JOB_BERRY) ++berryCount;
        else if (entry.second == FARMER_JOB_WOOD) ++woodCount;
        else if (entry.second == FARMER_JOB_STONE) ++stoneCount;
    }

    // 第一轮：尚未登记工作的开局村民 → 按 4/2/1/1 比例首次分配。
    for (int farmerSn : gAiState.initialFarmerSns) {
        if (gAiState.farmerJobs.count(farmerSn) != 0) continue;

        const tagFarmer* farmer = findFarmer(info, farmerSn);
        if (farmer == nullptr) continue;

        // 按优先级决定这名村民的职业：浆果未满先补浆果，其次木头，再石头。
        // 不预留建设者——建房时由 ensureBuilder 临时从伐木村民中选一人。
        int job = FARMER_JOB_UNASSIGNED;
        if (berryCount < TARGET_BERRY_WORKERS) {
            job = FARMER_JOB_BERRY; ++berryCount;
        } else if (woodCount < TARGET_WOOD_WORKERS + 1) {
            job = FARMER_JOB_WOOD; ++woodCount;
        } else if (stoneCount < TARGET_STONE_WORKERS) {
            job = FARMER_JOB_STONE; ++stoneCount;
        } else {
            job = FARMER_JOB_WOOD; ++woodCount;
        }

        // 采集类职业：找最近的对应资源点。
        const int resType = resourceTypeForJob(job);
        int targetSn = findNearestResource(info, *farmer, reservedTargets, resType, false);
        if (targetSn < 0) {
            // 附近资源点不足时允许多人采同一处，保证村民不会一直空闲。
            targetSn = findNearestResource(info, *farmer, reservedTargets, resType, true);
        }
        if (targetSn < 0) return; // 找不到资源，等下一帧再试。

        const int orderId = ai.HumanAction(farmerSn, targetSn);
        gAiState.farmerJobs[farmerSn] = job;
        gAiState.farmerGatherTargets[farmerSn] = targetSn;
        gAiState.pendingGatherOrders[orderId] = farmerSn;
        gAiState.farmerOrderFrame[farmerSn] = gAiState.lastFrame;
        aiLog(QString("[AI Gather First] order=%1 farmer=%2 target=%3 type=%4")
                   .arg(orderId)
                   .arg(farmerSn)
                   .arg(targetSn)
                   .arg(resourceNameForJob(job)));
        return;
    }

    // 第一轮半：新登记的村民（UNASSIGNED）按规则分工。
    // 总浆果数未满 TARGET_BERRY_TOTAL 时补浆果，超过后新村民去伐木。
    for (const auto& entry : gAiState.farmerJobs) {
        const int farmerSn = entry.first;
        const int currentJob = entry.second;
        if (currentJob != FARMER_JOB_UNASSIGNED) continue;

        const tagFarmer* farmer = findFarmer(info, farmerSn);
        if (farmer == nullptr) continue; // 死亡记录由 cleanupDeadFarmerRecords 处理

        // 决定新村民职业：浆果目标未满补浆果，否则去伐木。
        int newJob = FARMER_JOB_BERRY;
        if (berryCount >= TARGET_BERRY_TOTAL) {
            newJob = FARMER_JOB_WOOD;
        }
        ++berryCount;

        const int resType = resourceTypeForJob(newJob);
        int targetSn = findNearestResource(info, *farmer, reservedTargets, resType, false);
        if (targetSn < 0) targetSn = findNearestResource(info, *farmer, reservedTargets, resType, true);
        if (targetSn < 0) return; // 找不到资源，等下一帧再试。

        const int orderId = ai.HumanAction(farmerSn, targetSn);
        gAiState.farmerJobs[farmerSn] = newJob;
        gAiState.farmerGatherTargets[farmerSn] = targetSn;
        gAiState.pendingGatherOrders[orderId] = farmerSn;
        gAiState.farmerOrderFrame[farmerSn] = gAiState.lastFrame;
        aiLog(QString("[AI New Farmer Assigned] order=%1 farmer=%2 target=%3 type=%4 berryTotal=%5")
                   .arg(orderId).arg(farmerSn).arg(targetSn)
                   .arg(resourceNameForJob(newJob)).arg(berryCount));
        return;
    }

    // 第二轮：已登记为采集资源的村民（含开局村民和新村民），检查是否需要重派资源点。
    for (const auto& entry : gAiState.farmerJobs) {
        const int farmerSn = entry.first;
        const int job = entry.second;
        // 建设者和未分配村民不参与重派。
        if (job != FARMER_JOB_BERRY && job != FARMER_JOB_WOOD && job != FARMER_JOB_STONE) continue;

        // 下令后冷却期：村民状态可能还没切换到 WALKING/WORKING，跳过避免重复派单。
        const auto lastOrderIt = gAiState.farmerOrderFrame.find(farmerSn);
        if (lastOrderIt != gAiState.farmerOrderFrame.end() &&
            gAiState.lastFrame - lastOrderIt->second < FARMER_ORDER_COOLDOWN_FRAMES) {
            continue;
        }

        const tagFarmer* farmer = findFarmer(info, farmerSn);
        if (farmer == nullptr) continue; // 死亡记录由 cleanupDeadFarmerRecords 处理

        const int oldTargetSn =
            gAiState.farmerGatherTargets.count(farmerSn) != 0
                ? gAiState.farmerGatherTargets[farmerSn] : -1;

        if (!needsResourceReassign(info, *farmer, oldTargetSn,
                                    pendingFarmers.count(farmerSn) != 0)) {
            continue;
        }

        // 资源耗尽：重新找同类型最近资源点。
        const int resType = resourceTypeForJob(job);
        int targetSn = findNearestResource(info, *farmer, reservedTargets, resType, false);
        if (targetSn < 0) targetSn = findNearestResource(info, *farmer, reservedTargets, resType, true);
        if (targetSn < 0) return;

        // 新目标与旧目标相同：村民已经被派往该点，不需要重复下令。
        if (targetSn == oldTargetSn) continue;

        const int orderId = ai.HumanAction(farmerSn, targetSn);
        gAiState.farmerGatherTargets[farmerSn] = targetSn;
        gAiState.pendingGatherOrders[orderId] = farmerSn;
        gAiState.farmerOrderFrame[farmerSn] = gAiState.lastFrame;
        aiLog(QString("[AI Gather Reassign] order=%1 farmer=%2 oldTarget=%3 newTarget=%4 type=%5")
                   .arg(orderId)
                   .arg(farmerSn)
                   .arg(oldTargetSn)
                   .arg(targetSn)
                   .arg(resourceNameForJob(job)));
        return;
    }
}

} // namespace

void UsrAI::processData()
{
    // getInfo() 返回当前帧中 AI 可见的完整游戏快照。
    const tagInfo info = getInfo();
    const int frame = info.GameFrame;
    const bool firstSnapshot = gAiState.lastFrame < 0;

    // 帧号倒退说明游戏重新开始，需要清除上一局保存的状态。
    if (gAiState.lastFrame >= 0 && frame < gAiState.lastFrame) {
        resetRuntimeState();
    }

    gAiState.lastFrame = frame;
    refreshKeyObjects(info);
    refreshNearbyResources(info);
    captureInitialFarmers(info);
    processCenterProductionResult(info);
    processGatherOrderResults(info);
    registerNewFarmers(info);
    manageCenterProduction(*this, info);
    manageBuilding(*this, info);
    managePriest(*this, info);
    manageFarmerJobs(*this, info);

    // 每局第一次收到游戏快照时，记录可见建筑和军队的“类型:SN”，方便检查地图初始状态。
    if (firstSnapshot) {
        QStringList buildings;
        for (const tagBuilding& building : info.buildings) {
            buildings << QString("%1:%2").arg(building.Type).arg(building.SN);
        }
        QStringList armies;
        for (const tagArmy& army : info.armies) {
            armies << QString("%1:%2").arg(army.Sort).arg(army.SN);
        }
        qInfo().noquote()
            << QString("[AI Snapshot] frame=%1 buildings=[%2] armies=[%3]")
                   .arg(frame)
                   .arg(buildings.join(','))
                   .arg(armies.join(','));
    }

    // 四个关键对象首次全部找到后，输出一次完整的初始化结果。
    if (gAiState.initialized && !gAiState.initializationReported) {
        const QString message =
            QString("[AI Init] frame=%1 center=%2 priest=%3 stock=%4 granary=%5 towers=%6 base=(%7,%8) direction=(%9,%10)")
                .arg(frame)
                .arg(gAiState.centerSn)
                .arg(gAiState.priestSn)
                .arg(gAiState.stockSn)
                .arg(gAiState.granarySn)
                .arg(static_cast<int>(gAiState.arrowTowerSns.size()))
                .arg(gAiState.baseDR)
                .arg(gAiState.baseUR)
                .arg(gAiState.towardCenterDR)
                .arg(gAiState.towardCenterUR);
        qInfo().noquote() << message;
        DebugText(message);
        gAiState.initializationReported = true;
        gAiState.lastReportFrame = frame;
    }

    // 初始化完成后每隔 1500 帧输出一次简要状态，便于观察 AI 是否仍在正常运行。
    if (gAiState.initialized && frame - gAiState.lastReportFrame >= 1500) {
        DebugText(QString("[AI Status] frame=%1 center=%2 priest=%3 towers=%4")
                  .arg(frame)
                  .arg(gAiState.centerSn)
                  .arg(gAiState.priestSn)
                  .arg(static_cast<int>(gAiState.arrowTowerSns.size())));
        // 同时把资源数值和各职业登记数写到 ai_debug.log，便于命令行验证资源增长。
        int berryWorkers = 0, woodWorkers = 0, stoneWorkers = 0, builderWorkers = 0;
        for (const auto& entry : gAiState.farmerJobs) {
            if (entry.second == FARMER_JOB_BERRY) ++berryWorkers;
            else if (entry.second == FARMER_JOB_WOOD) ++woodWorkers;
            else if (entry.second == FARMER_JOB_STONE) ++stoneWorkers;
            else if (entry.second == FARMER_JOB_BUILDER) ++builderWorkers;
        }
        aiLog(QString("[AI Status] frame=%1 meat=%2 wood=%3 stone=%4 gold=%5 berry=%6 wood=%7 stone=%8 builder=%9")
                  .arg(frame)
                  .arg(info.Meat)
                  .arg(info.Wood)
                  .arg(info.Stone)
                  .arg(info.Gold)
                  .arg(berryWorkers)
                  .arg(woodWorkers)
                  .arg(stoneWorkers)
                  .arg(builderWorkers));
        gAiState.lastReportFrame = frame;
    }
}
