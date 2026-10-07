#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <QFile>
#include <QTextStream>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

// ====================== 1. 全局状态变量定义 ======================
int  g_aiframe = 0;

unordered_map<int,int> g_role;
int  g_builderSN = -1;
int  g_hunterSN[2] = {-1, -1};
bool g_hunterActive = false;
bool g_initDone = false;

int  g_defTowerSN = -1;
int  g_defTowerDR = -1, g_defTowerUR = -1;
int  g_tcDR = -1, g_tcUR = -1;
bool g_towerTechIssued = false;
bool g_towerTechBusy = false;
bool g_towerTechDone = false;

bool g_defenseMode = false;
int  g_calmFrames = 0;
bool g_wave1Handled = false;
bool g_wave2Handled = false;
int  g_activeDefenseWave = 0;

int  g_priestSN = -1;
int  g_priestScoutIdx = 0;
int  g_priestLastMoveFrame = -1000;
int  g_priestConvertTarget = -1;
double g_priestPointDR = 0, g_priestPointUR = 0;
double g_rallyDR = 0, g_rallyUR = 0;

unordered_map<int,int> g_lastBuildTry;
bool g_goldStockBuilt = false;

bool g_woodTechIssued = false;
bool g_woodTechBusy = false;
bool g_woodTechDone = false;
bool g_compositeIssued = false;
bool g_compositeTechBusy = false;
bool g_compositeTechDone = false;

set<int> g_orderedThisFrame;

struct PendingBuild { int builder, type, dr, ur, frame; };
static unordered_map<int, PendingBuild> g_pendingBuild; // order id -> build
static unordered_map<int, PendingBuild> g_acceptedBuild; // builder SN -> build
static set<int> g_failedBuildSites;
static unordered_map<int,int> g_farmerGatherTargets;
static unordered_map<int,int> g_gatherOrders;
static unordered_map<int,int> g_gatherOrderFrames;
static int g_farmerOrderId = -1;
static int g_houseCapacityRefreshUntil = -1;
static int g_farmerOrderBaseCount = -1;
static int g_farmerOrderFrame = -1;
static bool g_farmerCapLogged = false;
static int g_lastCivStage = -1;
static set<int> g_loggedCompletedBuildings;
static set<int> g_upgradeFoodWorkers;
static set<int> g_berryFarmWorkers;
// 农田续建单独等回执；明确无法续建的坏田不再占用六块可用田的名额。
static int g_farmResumeOrder = -1, g_farmResumeSN = -1;
static int g_farmResumeFrame = -1000;
static set<int> g_failedFarms;
static map<int,int> g_seenFarmFood;
static int g_huntTargetSN = -1;
static bool g_huntKillingLive = true;
static int g_hunterLastMoveFrame = -1000;
static bool g_hunterPairReady = false;
static set<int> g_huntHerdTargets;
static bool g_huntHerdCaptured = false;
static int g_huntIssuedTargetSN = -1;
static int g_huntLastAttackFrame = -1000;
static bool g_defenseApproachLocked = false;
static double g_defenseApproachDR = 0.0;
static double g_defenseApproachUR = 0.0;
static int g_towerLastAttackFrame = -1000;
static int g_towerLastAttackTarget = -1;
static set<int> g_towerAggroLogged;
static bool g_towerRepairStarted = false;
static bool g_towerRescueSweepActive = false;
static int g_towerNoPriestThreatFrames = 0;
static int g_lastCombatTowerRepairFrame = -1000;
static bool g_priestDangerRetreat = false;
static int g_priestDangerAttacker = -1;
static int g_priestDangerSafeFrames = 0;
static double g_priestRetreatDR = 0.0;
static double g_priestRetreatUR = 0.0;
static set<int> g_siegeInterceptors;
static int g_siegeInterceptTarget = -1;

enum RangedScoutState { RANGED_SCOUT_NONE, RANGED_SCOUT_EXPLORE,
                        RANGED_SCOUT_RETURN, RANGED_SCOUT_HOLD,
                        RANGED_SCOUT_LOST };
static int g_rangedScoutSN = -1;
static RangedScoutState g_rangedScoutState = RANGED_SCOUT_NONE;
static int g_rangedScoutPoint = 0;
static int g_rangedScoutLastMoveFrame = -1000;

// 防守逻辑在文件后部定义；双人猎需要用它避免覆盖被锁定村民的撤退命令。
static int enemyTargetingFarmer(const tagInfo& info, int farmerSN);
static bool isDangerousRangedEnemy(int sort);

// 发包辅助函数是文件级自由函数，不能直接调 AI 的成员函数，
// 用 processData 开头保存的 this 指针转发
static AI* g_ai = nullptr;
static void dbg(const QString& s)
{
    if(g_ai) g_ai->DebugText(s);
    static bool firstWrite = true;
    QFile file("ai_debug.log");
    QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text |
                               (firstWrite ? QIODevice::Truncate : QIODevice::Append);
    if(file.open(mode))
    {
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        stream << s << '\n';
        firstWrite = false;
    }
}

// ====================== 3. 基础辅助函数 ======================

// 细节坐标距离（平方，块坐标请先乘 BLOCKSIDELENGTH）
static inline double d2(double x1, double y1, double x2, double y2)
{
    double dx = x1 - x2, dy = y1 - y2;
    return dx*dx + dy*dy;
}
static inline double blockLen(int dr, int ur)
{
    return sqrt((double)dr*dr + (double)ur*ur);
}

const tagFarmer* usrGetFarmer(const tagInfo& info, int sn)
{
    for(const auto& f : info.farmers)
        if(f.SN == sn) return &f;
    return nullptr;
}
const tagArmy* usrGetArmy(const tagInfo& info, int sn)
{
    for(const auto& a : info.armies)
        if(a.SN == sn) return &a;
    return nullptr;
}

// 该类型建筑是否已存在（含在建 Percent>0）——用于防止重复下单
bool usrHasBuilding(const tagInfo& info, int type)
{
    for(const auto& b : info.buildings)
        if(b.Type == type && b.Percent > 0) return true;
    return false;
}
// 已建成（Percent>=100）的该类型建筑数量
int usrCountBuilt(const tagInfo& info, int type)
{
    int n = 0;
    for(const auto& b : info.buildings)
        if(b.Type == type && b.Percent >= 100) n++;
    return n;
}

// 以(cDR,cUR)块为中心、半径 [minR,maxR] 环上找一块3x3可建平地
// 8个方向轮流作为搜索起点（静态旋转量），避免连续建造死磕同一格
// 地形要求：3x3全草地(非海洋)、高度一致、不压已有建筑/静态资源
bool usrFindFlatNear(int &outDR, int &outUR, const tagInfo& info,
                     int cDR, int cUR, int minR, int maxR)
{
    if(info.theMap == nullptr) return false;
    int rows = (int)info.theMap->size();
    if(rows == 0) return false;
    int cols = (int)(*info.theMap)[0].size();
    if(cols == 0) return false;

    static int rotation = 0;
    for(int r = minR; r <= maxR; r++)
    {
        vector<pair<int,int>> ring;
        for(int d=-r; d<=r; ++d)
        {
            ring.push_back({d,-r}); ring.push_back({d,r});
            if(abs(d) != r) { ring.push_back({-r,d}); ring.push_back({r,d}); }
        }
        if(!ring.empty()) rotation %= (int)ring.size();
        for(size_t k=0; k<ring.size(); ++k)
        {
            const auto& offset = ring[(k + rotation) % ring.size()];
            int dr = cDR + offset.first, ur = cUR + offset.second;
            if(dr < 2 || ur < 2 || dr >= rows-2 || ur >= cols-2) continue;
            if(g_failedBuildSites.count(dr * 1000 + ur)) continue;

            bool ok = true;
            for(int i=-1; i<=1 && ok; i++)
                for(int j=-1; j<=1 && ok; j++)
                {
                    int t = (*info.theMap)[dr+i][ur+j].type;
                    if(t != MAPPATTERN_GRASS) ok = false;   // 海洋/沙漠等一律不建
                }
            if(!ok) continue;

            int h = (*info.theMap)[dr][ur].height;
            for(int i=-1; i<=1 && ok; i++)
                for(int j=-1; j<=1 && ok; j++)
                    if((*info.theMap)[dr+i][ur+j].height != h) ok = false;
            if(!ok) continue;

            // 市镇中心附近保留十字通道，避免后期建筑围住交货路线。
            if(g_tcDR >= 0 && g_tcUR >= 0 &&
               usrBlockDist(dr, ur, g_tcDR, g_tcUR) <= 14.0 &&
               (abs(dr-g_tcDR) <= 2 || abs(ur-g_tcUR) <= 2))
                continue;

            bool busy = false;
            // 建筑中心至少相隔4格。建筑通常占3x3，这会在建筑之间留出通行空间。
            for(const auto& b : info.buildings)           // 避开所有建筑(含在建)
                if(abs(b.BlockDR-dr) <= 3 && abs(b.BlockUR-ur) <= 3) { busy = true; break; }
            if(busy) continue;
            for(const auto& rs : info.resources)          // 只避开静态资源，动物会走动不拦
            {
                if(rs.Type != RESOURCE_TREE && rs.Type != RESOURCE_BUSH &&
                   rs.Type != RESOURCE_STONE && rs.Type != RESOURCE_GOLD) continue;
                if(rs.BlockDR < 0 || rs.BlockUR < 0) continue;
                if(abs(rs.BlockDR-dr) <= 1 && abs(rs.BlockUR-ur) <= 1) { busy = true; break; }
            }
            if(busy) continue;

            outDR = dr; outUR = ur;
            rotation = (rotation + 7) % (int)ring.size();
            return true;
        }
    }
    return false;
}

// 找离(fromDR,fromUR)最近的某类资源SN；liveOnly=true 时只要活体动物(Blood>0)
int usrNearestResource(const tagInfo& info, int resType,
                       double fromDR, double fromUR, bool liveOnly)
{
    int best = -1;
    double bd = 1e30;
    for(const auto& r : info.resources)
    {
        if(r.Type != resType) continue;
        if(liveOnly && r.Blood <= 0) continue;
        if(!liveOnly && r.Cnt <= 0 && r.Blood <= 0) continue;
        double d = d2(fromDR, fromUR, r.DR, r.UR);
        if(d < bd) { bd = d; best = r.SN; }
    }
    return best;
}

int usrCountLandFarmers(const tagInfo& info)
{
    int count = 0;
    for(const auto& farmer : info.farmers)
        if(farmer.FarmerSort == FARMERTYPE_FARMER) ++count;
    return count;
}

static bool usableResource(const tagInfo& info, int sn, int type = -1)
{
    for(const auto& resource : info.resources)
        if(resource.SN == sn && (type < 0 || resource.Type == type))
            return resource.Cnt > 0 || resource.Blood > 0;
    return false;
}

static bool usableFarmTarget(const tagInfo& info, int sn)
{
    for(const auto& building : info.buildings)
        if(building.SN == sn && building.Type == BUILDING_FARM &&
           building.Percent >= 100 && building.Cnt > 0)
            return true;
    return false;
}

static void cleanupGatherTargets(const tagInfo& info)
{
    for(auto it=g_farmerGatherTargets.begin(); it!=g_farmerGatherTargets.end(); )
    {
        const tagFarmer* farmer = usrGetFarmer(info, it->first);
        if(!farmer ||
           (!usableResource(info, it->second) && !usableFarmTarget(info, it->second)) ||
           it->first == g_builderSN)
            it = g_farmerGatherTargets.erase(it);
        else ++it;
    }
}

static void cleanupFarmerRoles(const tagInfo& info)
{
    for(auto& entry : g_role)
        if(!usrGetFarmer(info, entry.first)) entry.second = ROLE_NONE;
    for(auto it=g_berryFarmWorkers.begin(); it!=g_berryFarmWorkers.end(); )
        if(!usrGetFarmer(info, *it)) it = g_berryFarmWorkers.erase(it);
        else ++it;
}

int usrBestGatherResource(const tagInfo& info, int resType, const tagFarmer& farmer)
{
    unordered_map<int,int> load;
    for(const auto& other : info.farmers)
    {
        if(other.FarmerSort != FARMERTYPE_FARMER || other.SN == g_builderSN) continue;
        int target = usableResource(info, other.WorkObjectSN, resType) ? other.WorkObjectSN : -1;
        auto saved = g_farmerGatherTargets.find(other.SN);
        if(target < 0 && saved != g_farmerGatherTargets.end() &&
           usableResource(info, saved->second, resType)) target = saved->second;
        if(target >= 0) ++load[target];
    }

    // 先在本地范围内分散。直接在全地图优先“无人资源”会把村民派到
    // 另一片大陆；20格内没有资源时再扩大到35格，最后才全图兜底。
    const double radii[] = {20.0, 35.0, 1e30};
    for(double radius : radii)
    {
        int best = -1, bestLoad = INT_MAX;
        double bestDistance = 1e30;
        double maxDistance = radius * BLOCKSIDELENGTH;
        maxDistance *= maxDistance;
        for(const auto& resource : info.resources)
        {
            if(resource.Type != resType || (resource.Cnt <= 0 && resource.Blood <= 0)) continue;
            double distance = d2(farmer.DR, farmer.UR, resource.DR, resource.UR);
            if(distance > maxDistance) continue;
            int resourceLoad = load[resource.SN];
            if(resourceLoad < bestLoad || (resourceLoad == bestLoad && distance < bestDistance))
            {
                best = resource.SN;
                bestLoad = resourceLoad;
                bestDistance = distance;
            }
        }
        if(best >= 0) return best;
    }
    return -1;
}

// 找离某细节坐标最近的某类建筑SN
int usrNearestBuilding(const tagInfo& info, int type, double fromDR, double fromUR)
{
    int best = -1;
    double bd = 1e30;
    for(const auto& b : info.buildings)
    {
        if(b.Type != type || b.Percent <= 0) continue;
        double bx = b.BlockDR * BLOCKSIDELENGTH, by = b.BlockUR * BLOCKSIDELENGTH;
        double d = d2(fromDR, fromUR, bx, by);
        if(d < bd) { bd = d; best = b.SN; }
    }
    return best;
}

double usrBlockDist(int aDR, int aUR, int bDR, int bUR)
{
    return blockLen(aDR-bDR, aUR-bUR);
}

// 取某SN资源指针
static const tagResource* getRes(const tagInfo& info, int sn)
{
    for(const auto& r : info.resources) if(r.SN == sn) return &r;
    return nullptr;
}
// 取某SN建筑指针
static const tagBuilding* getBuilding(const tagInfo& info, int sn)
{
    for(const auto& b : info.buildings)
        if(b.SN == sn) return &b;
    return nullptr;
}

// ---- 每帧"同一SN只下一条命令"的四个安全发包封装 ----
static bool snFree(int sn) { return g_orderedThisFrame.count(sn) == 0; }
static void markSN(int sn) { g_orderedThisFrame.insert(sn); }

static void orderMove(int sn, double dr, double ur)
{
    if(!snFree(sn)) return;
    g_ai->HumanMove(sn, dr, ur);
    markSN(sn);
}
static void orderAction(const tagInfo& info, int sn, int objSN)
{
    if(!snFree(sn)) return;
    int order = g_ai->HumanAction(sn, objSN);
    if(usrGetFarmer(info, sn))
    {
        g_farmerGatherTargets[sn] = objSN;
        g_gatherOrders[order] = sn;
        g_gatherOrderFrames[order] = g_aiframe;
    }
    markSN(sn);
}
// 让建筑工继续施工或修理受损建筑。这里不登记为采集目标，避免污染普通村民分配。
static void orderBuilderWork(int sn, int buildingSN)
{
    if(!snFree(sn)) return;
    g_ai->HumanAction(sn, buildingSN);
    markSN(sn);
}
static void orderBuild(int sn, int bType, int bDR, int bUR)
{
    if(!snFree(sn) || g_acceptedBuild.count(sn)) return;
    for(const auto& p : g_pendingBuild) if(p.second.builder == sn) return;
    int order = g_ai->HumanBuild(sn, bType, bDR, bUR);
    g_pendingBuild[order] = {sn, bType, bDR, bUR, g_aiframe};
    markSN(sn);
}
static void orderBldAction(int sn, int act)
{
    if(!snFree(sn)) return;
    g_ai->BuildingAction(sn, act);
    markSN(sn);
}

static void processBuildResults(const tagInfo& info)
{
    for(auto it=g_pendingBuild.begin(); it!=g_pendingBuild.end(); )
    {
        auto result = info.ins_ret.find(it->first);
        if(result == info.ins_ret.end() && g_aiframe-it->second.frame < 300) { ++it; continue; }
        PendingBuild build = it->second;
        if(result != info.ins_ret.end() && result->second == ACTION_SUCCESS)
        {
            g_acceptedBuild[build.builder] = build;
            dbg(QString("[建造接受] order=%1 builder=%2 type=%3 position=(%4,%5)")
                      .arg(it->first).arg(build.builder).arg(build.type)
                      .arg(build.dr).arg(build.ur));
        }
        else
        {
            g_failedBuildSites.insert(build.dr*1000 + build.ur);
            dbg(QString("[建造恢复] 指令失败，跳过位置(%1,%2)").arg(build.dr).arg(build.ur));
        }
        it = g_pendingBuild.erase(it);
    }

    for(auto it=g_acceptedBuild.begin(); it!=g_acceptedBuild.end(); )
    {
        bool appeared = false;
        for(const auto& b : info.buildings)
            if(b.Type==it->second.type && b.BlockDR==it->second.dr && b.BlockUR==it->second.ur)
            { appeared=true; break; }
        if(appeared)
        {
            dbg(QString("[建筑出现] builder=%1 type=%2 position=(%3,%4)")
                      .arg(it->second.builder).arg(it->second.type)
                      .arg(it->second.dr).arg(it->second.ur));
            it = g_acceptedBuild.erase(it);
        }
        else if(g_aiframe-it->second.frame >= 300)
        {
            g_failedBuildSites.insert(it->second.dr*1000 + it->second.ur);
            dbg(QString("[建造恢复] 建筑未出现，跳过位置(%1,%2)")
                      .arg(it->second.dr).arg(it->second.ur));
            it = g_acceptedBuild.erase(it);
        }
        else ++it;
    }
}

static void logEconomicProgress(const tagInfo& info)
{
    for(auto it = g_seenFarmFood.begin(); it != g_seenFarmFood.end(); )
    {
        const tagBuilding* farm = getBuilding(info, it->first);
        if(!farm || farm->Cnt <= 0)
        {
            dbg(QString("[农田失效] farm=%1 lastFood=%2 reason=%3，等待补建")
                      .arg(it->first).arg(it->second)
                      .arg(farm ? QString("exhausted") : QString("removed")));
            it = g_seenFarmFood.erase(it);
        }
        else { it->second = farm->Cnt; ++it; }
    }
    if(g_lastCivStage < 0) g_lastCivStage = info.civilizationStage;
    else if(info.civilizationStage > g_lastCivStage)
    {
        dbg(QString("[时代升级完成] stage=%1 frame=%2")
                  .arg(info.civilizationStage).arg(g_aiframe));
        g_lastCivStage = info.civilizationStage;
    }

    for(const auto& building : info.buildings)
    {
        if(building.Percent < 100 || g_loggedCompletedBuildings.count(building.SN)) continue;
        if(building.Type != BUILDING_HOME && building.Type != BUILDING_ARMYCAMP &&
           building.Type != BUILDING_MARKET && building.Type != BUILDING_RANGE &&
           building.Type != BUILDING_FARM) continue;
        g_loggedCompletedBuildings.insert(building.SN);
        if(building.Type == BUILDING_FARM)
        {
            g_seenFarmFood[building.SN] = building.Cnt;
            dbg(QString("[农田完成] farm=%1 food=%2 frame=%3")
                      .arg(building.SN).arg(building.Cnt).arg(g_aiframe));
            continue;
        }
        dbg(QString("[建筑完成] type=%1 sn=%2 frame=%3")
                  .arg(building.Type).arg(building.SN).arg(g_aiframe));
        if(building.Type == BUILDING_HOME)
        {
            // 房屋Percent先变成100，Human_MaxNum要到后续游戏帧才刷新。
            // 短暂等待，避免在同一份旧快照上误判为仍然人口锁死并再造一座。
            g_houseCapacityRefreshUntil = g_aiframe + 10;
            dbg(QString("[人口房刷新等待] house=%1 untilFrame=%2")
                      .arg(building.SN).arg(g_houseCapacityRefreshUntil));
        }
    }
}

static void processFarmResumeResult(const tagInfo& info)
{
    for(auto it = g_failedFarms.begin(); it != g_failedFarms.end(); )
        if(!getBuilding(info, *it) || getBuilding(info, *it)->Percent >= 100)
            it = g_failedFarms.erase(it);
        else ++it;
    if(g_farmResumeOrder < 0) return;
    auto result = info.ins_ret.find(g_farmResumeOrder);
    if(result == info.ins_ret.end() && g_aiframe-g_farmResumeFrame < 300) return;
    int code = result == info.ins_ret.end() ? -1 : result->second;
    dbg(QString("[农田续建结果] order=%1 farm=%2 result=%3")
              .arg(g_farmResumeOrder).arg(g_farmResumeSN).arg(code));
    const tagBuilding* farm = getBuilding(info, g_farmResumeSN);
    if(code == ACTION_INVALID_HUMANACTION_BUILDNOTNEEDFIX &&
       farm && farm->Percent < 100)
    {
        g_failedFarms.insert(farm->SN);
        dbg(QString("[农田恢复] farm=%1 percent=%2 blood=%3/%4，接口拒绝续建，补建替代田")
                  .arg(farm->SN).arg(farm->Percent).arg(farm->Blood).arg(farm->MaxBlood));
    }
    g_farmResumeOrder = -1;
}

static void resumeFarm(const tagFarmer& builder, const tagBuilding& farm)
{
    if(g_farmResumeOrder >= 0 || !snFree(builder.SN) ||
       (builder.WorkObjectSN == farm.SN && builder.NowState != HUMAN_STATE_IDLE) ||
       g_aiframe-g_farmResumeFrame < 100) return;
    g_farmResumeOrder = g_ai->HumanAction(builder.SN, farm.SN);
    g_farmResumeSN = farm.SN;
    g_farmResumeFrame = g_aiframe;
    markSN(builder.SN);
    dbg(QString("[农田续建下单] order=%1 builder=%2 farm=%3 percent=%4 blood=%5/%6")
              .arg(g_farmResumeOrder).arg(builder.SN).arg(farm.SN)
              .arg(farm.Percent).arg(farm.Blood).arg(farm.MaxBlood));
}

static void processGatherResults(const tagInfo& info)
{
    for(auto it=g_gatherOrders.begin(); it!=g_gatherOrders.end(); )
    {
        auto result = info.ins_ret.find(it->first);
        if(result == info.ins_ret.end() &&
           g_aiframe-g_gatherOrderFrames[it->first] < 300) { ++it; continue; }
        bool timedOut = result == info.ins_ret.end();
        if(timedOut || result->second != ACTION_SUCCESS)
        {
            g_farmerGatherTargets.erase(it->second);
            if(timedOut)
                dbg(QString("[采集恢复] 村民%1 指令结果超时").arg(it->second));
            else
                dbg(QString("[采集恢复] 村民%1 指令失败 code=%2")
                          .arg(it->second).arg(result->second));
        }
        g_gatherOrderFrames.erase(it->first);
        it = g_gatherOrders.erase(it);
    }
}

// 找一片"已建成、有余粮、且还没有农民在种"的农田SN
// （农田只能1人种，多个人挤同一块田会被内核拒绝，表现为站着不动）
static int findFreeFarm(const tagInfo& info, int farmerSN = -1)
{
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_FARM || b.Percent < 100 || b.Cnt <= 0) continue;
        bool occupied = false;
        for(const auto& f : info.farmers)
            if(f.Blood > 0 && f.SN != farmerSN && f.WorkObjectSN == b.SN)
            { occupied = true; break; }
        if(!occupied)
            for(const auto& target : g_farmerGatherTargets)
            {
                const tagFarmer* owner = usrGetFarmer(info, target.first);
                if(target.first != farmerSN && owner && owner->Blood > 0 &&
                   target.second == b.SN) { occupied = true; break; }
            }
        if(!occupied) return b.SN;
    }
    return -1;
}

static int berryFoodNearBase(const tagInfo& info);
static bool isCompletedFarm(const tagInfo& info, int sn);
static int chooseBerryWorkerFoodTarget(const tagInfo& info, const tagFarmer& farmer);
static bool farmerHasPendingGatherOrder(int sn);

// 某类建造上次尝试帧（没记录过返回 -99999）
static int lastTry(int key)
{
    auto it = g_lastBuildTry.find(key);
    return it == g_lastBuildTry.end() ? -99999 : it->second;
}

// ====================== 4. 阵地初始化（TC / 箭塔 / 集结点） ======================
static void setupBase(const tagInfo& info)
{
    // 定位TC
    for(const auto& b : info.buildings)
        if(b.Type == BUILDING_CENTER) { g_tcDR = b.BlockDR; g_tcUR = b.BlockUR; break; }
    if(g_tcDR < 0) return;

    double tcx = g_tcDR * BLOCKSIDELENGTH, tcy = g_tcUR * BLOCKSIDELENGTH;

    // 找离TC最近的箭塔（预置的那座）
    int towerSN = usrNearestBuilding(info, BUILDING_ARROWTOWER, tcx, tcy);
    g_defTowerSN = towerSN;
    if(towerSN != -1)
    {
        for(const auto& b : info.buildings)
            if(b.SN == towerSN) { g_defTowerDR = b.BlockDR; g_defTowerUR = b.BlockUR; break; }
    }

    // 第一波坚持使用预置箭塔。此点仅作为祭司/士兵的阵地参考；
    // 普通村民不会再被统一召集到这里。
    int rallyCenterDR = towerSN != -1 ? g_defTowerDR : g_tcDR;
    int rallyCenterUR = towerSN != -1 ? g_defTowerUR : g_tcUR;
    int rx, ry;
    if(usrFindFlatNear(rx, ry, info, rallyCenterDR, rallyCenterUR, 2, 5))
    {
        g_rallyDR = rx * BLOCKSIDELENGTH;
        g_rallyUR = ry * BLOCKSIDELENGTH;
    }
    else
    {
        if(towerSN != -1)
        {
            double tx = g_defTowerDR*BLOCKSIDELENGTH;
            double ty = g_defTowerUR*BLOCKSIDELENGTH;
            double dx = tcx-tx, dy = tcy-ty;
            double len = sqrt(dx*dx+dy*dy);
            if(len < 0.5) len = 1.0;
            g_rallyDR = tx + dx/len*3.0*BLOCKSIDELENGTH;
            g_rallyUR = ty + dy/len*3.0*BLOCKSIDELENGTH;
        }
        else
        {
            g_rallyDR = tcx + 2*BLOCKSIDELENGTH;
            g_rallyUR = tcy + 2*BLOCKSIDELENGTH;
        }
    }
    dbg(QString("[防守] 阵地参考点=(%1,%2)，防守塔SN=%3")
              .arg(g_rallyDR/BLOCKSIDELENGTH,0,'f',1)
              .arg(g_rallyUR/BLOCKSIDELENGTH,0,'f',1).arg(g_defTowerSN));
}

// 计算祭司/士兵的"塔后安全点"。防守开始后，“后方”取第一波实际来敌方向
// 的反方向；尚未发现敌军时，才以箭塔朝TC的方向作为默认后方。
static void refreshPriestPoint()
{
    if(g_defTowerSN == -1)
    {
        g_priestPointDR = g_rallyDR;
        g_priestPointUR = g_rallyUR;
        return;
    }
    double tx = g_defTowerDR * BLOCKSIDELENGTH;
    double ty = g_defTowerUR * BLOCKSIDELENGTH;
    double tcx = g_tcDR * BLOCKSIDELENGTH, tcy = g_tcUR * BLOCKSIDELENGTH;
    double dx = tcx - tx, dy = tcy - ty;
    if(g_defenseMode && g_defenseApproachLocked)
    {
        dx = tx - g_defenseApproachDR;
        dy = ty - g_defenseApproachUR;
    }
    double L = sqrt(dx*dx + dy*dy);
    if(L < 0.5)
    {
        g_priestPointDR = tcx + PRIEST_BEHIND_TOWER_BLOCKS*BLOCKSIDELENGTH;
        g_priestPointUR = tcy;
        return;
    }
    // 从塔向TC方向挪4格，让箭塔站在祭司与敌军之间。
    g_priestPointDR = tx + dx / L * PRIEST_BEHIND_TOWER_BLOCKS * BLOCKSIDELENGTH;
    g_priestPointUR = ty + dy / L * PRIEST_BEHIND_TOWER_BLOCKS * BLOCKSIDELENGTH;
}

// 若TC附近出现另一座已建成箭塔，把主防塔切换到近塔
static void refreshDefTower(const tagInfo& info)
{
    double tcx = g_tcDR * BLOCKSIDELENGTH, tcy = g_tcUR * BLOCKSIDELENGTH;
    int nearSN = -1; double best = 8*8;
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
        double bx = b.BlockDR*BLOCKSIDELENGTH, by = b.BlockUR*BLOCKSIDELENGTH;
        double dd = d2(tcx, tcy, bx, by) / (BLOCKSIDELENGTH*BLOCKSIDELENGTH);
        if(dd < best) { best = dd; nearSN = b.SN; }
    }
    if(nearSN != -1 && nearSN != g_defTowerSN)
    {
        g_defTowerSN = nearSN;
        for(const auto& b : info.buildings)
            if(b.SN == nearSN) { g_defTowerDR = b.BlockDR; g_defTowerUR = b.BlockUR; break; }
        dbg(QString("[防守] 主防塔切换至近塔(%1,%2)")
                  .arg(g_defTowerDR).arg(g_defTowerUR));
    }
}

// ====================== 5. 开局8人初始分配 ======================
static void initialAssign(const tagInfo& info)
{
    if(g_initDone) return;
    vector<const tagFarmer*> lands;
    for(const auto& f : info.farmers)
        if(f.FarmerSort == FARMERTYPE_FARMER) lands.push_back(&f);
    if(lands.size() < 8) return;   // 人没到齐（开局应正好8个）

    int idx = 0;
    auto assign = [&](int sn, int role) { g_role[sn] = role; };

    // (1) 1名专职建筑工
    g_builderSN = lands[idx++]->SN;
    assign(g_builderSN, ROLE_BUILDER);

    // (2) 3名伐木（具体砍哪棵树在后面统一按本人位置下令）
    for(int i=0; i<3; i++)
    {
        int sn = lands[idx++]->SN;
        assign(sn, ROLE_WOOD);
    }
    // (3) 开局4名食物工全部采浆果。猎人只从后续生产的村民中选择，
    // 避免开局村民为了追逐瞪羚而打乱稳定的食物收入。
    for(; idx < 8; idx++)
    {
        int sn = lands[idx]->SN;
        assign(sn, ROLE_BUSH);
        g_berryFarmWorkers.insert(sn);
    }

    // 立即下达第一份工作指令（之后由"空闲续任务"兜底）
    for(const auto& f : info.farmers)
    {
        if(f.FarmerSort != FARMERTYPE_FARMER) continue;
        int role = g_role.count(f.SN) ? g_role[f.SN] : ROLE_NONE;
        if(role == ROLE_WOOD)
        {
            int t = usrBestGatherResource(info, RESOURCE_TREE, f);
            if(t != -1) orderAction(info, f.SN, t);
        }
        else if(role == ROLE_BUSH)
        {
            int t = usrBestGatherResource(info, RESOURCE_BUSH, f);
            if(t != -1) orderAction(info, f.SN, t);
        }
    }
    g_initDone = true;
    dbg(QString("[开局] 初始分工完成：1建筑工+3伐木+4采果（猎人由后续村民担任）"));
}

// ====================== 6. 新出生村民分配（每帧最多1个） ======================
static int countRole(int role)
{
    int n = 0;
    for(auto& kv : g_role) if(kv.second == role) n++;
    return n;
}
// 金矿区附近8格内是否已有仓库（含在建）
static bool stockNearGold(const tagInfo& info)
{
    // 以TC为基准找最近金矿（金矿与TC同大陆，17~20格）
    double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
    int goldSN = usrNearestResource(info, RESOURCE_GOLD, tcx, tcy, false);
    if(goldSN == -1) return false;
    const tagResource* g = getRes(info, goldSN);
    if(!g) return false;
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_STOCK || b.Percent < 100) continue;
        if(usrBlockDist(b.BlockDR, b.BlockUR, g->BlockDR, g->BlockUR) <= 8.0) return true;
    }
    return false;
}

// 只选择基地附近、仍在25格追猎范围内的活体瞪羚，防止猎人跑遍整张地图。
static int findLiveHuntTarget(const tagInfo& info, const tagFarmer& leader)
{
    int best = -1;
    double bestDistance = 1e30;
    for(const auto& resource : info.resources)
    {
        if(resource.Type != RESOURCE_GAZELLE || resource.Blood <= 0) continue;
        if(usrBlockDist(resource.BlockDR, resource.BlockUR, g_tcDR, g_tcUR) >
           HUNT_CHASE_LIMIT) continue;
        double distance = d2(leader.DR, leader.UR, resource.DR, resource.UR);
        if(distance < bestDistance)
        {
            best = resource.SN;
            bestDistance = distance;
        }
    }
    return best;
}

// 两人会合时固定记录附近猎物群，之后不会在杀完活体前改采尸体。
static void captureHuntHerd(const tagInfo& info)
{
    g_huntHerdTargets.clear();
    for(const auto& resource : info.resources)
        if(resource.Type == RESOURCE_GAZELLE && resource.Blood > 0 &&
           usrBlockDist(resource.BlockDR, resource.BlockUR, g_tcDR, g_tcUR) <=
               HUNT_CHASE_LIMIT)
            g_huntHerdTargets.insert(resource.SN);

    const tagResource* reserved = getRes(info, g_huntTargetSN);
    if(reserved && reserved->Type == RESOURCE_GAZELLE && reserved->Blood > 0 &&
       usrBlockDist(reserved->BlockDR, reserved->BlockUR, g_tcDR, g_tcUR) <=
           HUNT_CHASE_LIMIT)
        g_huntHerdTargets.insert(reserved->SN);

    g_huntHerdCaptured = true;
    dbg(QString("[打猎猎物群] 已记录%1只活体瞪羚").arg(g_huntHerdTargets.size()));
}

// 从已经记录的猎物群中选择下一只；死亡、消失或逃出25格的目标才移出名单。
static int findTrackedLiveHuntTarget(const tagInfo& info, const tagFarmer& leader)
{
    int best = -1;
    double bestDistance = 1e30;
    for(auto it=g_huntHerdTargets.begin(); it!=g_huntHerdTargets.end(); )
    {
        const tagResource* resource = getRes(info, *it);
        if(!resource || resource->Type != RESOURCE_GAZELLE || resource->Blood <= 0)
        {
            it = g_huntHerdTargets.erase(it);
            continue;
        }
        if(usrBlockDist(resource->BlockDR, resource->BlockUR, g_tcDR, g_tcUR) >
           HUNT_CHASE_LIMIT)
        {
            dbg(QString("[打猎放弃] 瞪羚%1逃出追赶范围").arg(resource->SN));
            it = g_huntHerdTargets.erase(it);
            continue;
        }
        double distance = d2(leader.DR, leader.UR, resource->DR, resource->UR);
        if(distance < bestDistance)
        {
            best = resource->SN;
            bestDistance = distance;
        }
        ++it;
    }
    return best;
}

// 活体清理完后选择仍有食物且当前负载最低的瞪羚尸体，避免多人挤在一具尸体上。
static int findHuntCarcass(const tagInfo& info, const tagFarmer& leader)
{
    unordered_map<int,int> load;
    for(const auto& farmer : info.farmers)
    {
        const tagResource* target = getRes(info, farmer.WorkObjectSN);
        if(target && target->Type == RESOURCE_GAZELLE &&
           target->Blood <= 0 && target->Cnt > 0)
            ++load[target->SN];
    }

    int best = -1, bestLoad = INT_MAX;
    double bestDistance = 1e30;
    for(const auto& resource : info.resources)
    {
        if(resource.Type != RESOURCE_GAZELLE || resource.Blood > 0 || resource.Cnt <= 0)
            continue;
        if(usrBlockDist(resource.BlockDR, resource.BlockUR, g_tcDR, g_tcUR) >
           HUNT_CHASE_LIMIT) continue;
        double distance = d2(leader.DR, leader.UR, resource.DR, resource.UR);
        int resourceLoad = load[resource.SN];
        if(resourceLoad < bestLoad ||
           (resourceLoad == bestLoad && distance < bestDistance))
        {
            best = resource.SN;
            bestLoad = resourceLoad;
            bestDistance = distance;
        }
    }
    return best;
}

static void assignNewFarmer(const tagInfo& info)
{
    // 找第一个还没岗位的陆地村民
    int sn = -1;
    for(const auto& f : info.farmers)
    {
        if(f.FarmerSort != FARMERTYPE_FARMER) continue;
        auto it = g_role.find(f.SN);
        if(it == g_role.end() || it->second == ROLE_NONE) { sn = f.SN; break; }
    }
    if(sn == -1) return;

    const tagFarmer* fp = usrGetFarmer(info, sn);
    if(!fp) return;

    int role;
    int foodN = countRole(ROLE_BUSH) + countRole(ROLE_HUNTER) + countRole(ROLE_FARM);
    int woodN = countRole(ROLE_WOOD);
    // 专职 builder 阵亡时，先从新补出的无岗位村民中恢复 builder。
    if(g_builderSN < 0 || !usrGetFarmer(info, g_builderSN))
    {
        g_builderSN = sn;
        role = ROLE_BUILDER;
    }
    // 固定保留6名“浆果->猎物尸体->农田”食物工；其余按双人猎、伐木分配。
    // 第一名只记录猎物并等搭档；第二名固定加入，不因猎物暂时失效而转去伐木。
    else if((int)g_berryFarmWorkers.size() < TARGET_BERRY_FARM_WORKERS)
        role = ROLE_BUSH;
    else if(countRole(ROLE_HUNTER) < 2)
    {
        const tagFarmer* firstHunter = usrGetFarmer(info, g_hunterSN[0]);
        int gazelle = firstHunter ? g_huntTargetSN : findLiveHuntTarget(info, *fp);
        role = firstHunter || gazelle >= 0 ? ROLE_HUNTER : ROLE_WOOD;
        if(!firstHunter) g_huntTargetSN = gazelle;
    }
    else role = ROLE_WOOD;

    g_role[sn] = role;
    if(role == ROLE_BUSH && (int)g_berryFarmWorkers.size() < TARGET_BERRY_FARM_WORKERS)
        g_berryFarmWorkers.insert(sn);

    if(role == ROLE_HUNTER)
    {
        if(g_hunterSN[0] < 0 || !usrGetFarmer(info, g_hunterSN[0]))
        {
            g_hunterSN[0] = sn;
            g_hunterSN[1] = -1;
            g_huntKillingLive = true;
            g_hunterPairReady = false;
            g_huntHerdTargets.clear();
            g_huntHerdCaptured = false;
            g_huntIssuedTargetSN = -1;
            dbg(QString("[打猎等待] 第一名猎人=%1 预留猎物=%2，原地等待搭档")
                      .arg(sn).arg(g_huntTargetSN));
        }
        else
        {
            g_hunterSN[1] = sn;
            const tagFarmer* firstHunter = usrGetFarmer(info, g_hunterSN[0]);
            if(firstHunter)
            {
                orderMove(sn, firstHunter->DR, firstHunter->UR);
                g_hunterLastMoveFrame = g_aiframe;
            }
            dbg(QString("[打猎集合] 第二名猎人=%1 前往第一名猎人=%2")
                      .arg(sn).arg(g_hunterSN[0]));
        }
        g_hunterActive = g_hunterSN[0] >= 0 && g_hunterSN[1] >= 0;
    }

    // 立即派活
    int t = -1;
    if(role == ROLE_WOOD)        t = usrBestGatherResource(info, RESOURCE_TREE, *fp);
    else if(role == ROLE_BUSH)
    {
        t = chooseBerryWorkerFoodTarget(info, *fp);
        if(isCompletedFarm(info, t)) role = ROLE_FARM;
        if(t < 0)
        {
            // 食物目标暂时不存在时先伐木，但仍保留其核心食物工身份；
            // 浆果、尸体或农田可用后会重新转回食物。
            t = usrBestGatherResource(info, RESOURCE_TREE, *fp);
        }
    }
    else if(role == ROLE_HUNTER) t = g_huntTargetSN; // 仅用于日志，会合前不攻击
    else if(role == ROLE_GOLD)   t = usrBestGatherResource(info, RESOURCE_GOLD, *fp);
    else if(role == ROLE_STONE)  t = usrBestGatherResource(info, RESOURCE_STONE, *fp);
    else if(role == ROLE_FARM)
    {
        t = findFreeFarm(info);                          // 只去没人种的田
        if(t == -1) t = usrBestGatherResource(info, RESOURCE_BUSH, *fp);
    }
    g_role[sn] = role;
    if(t != -1 && role != ROLE_HUNTER) orderAction(info, sn, t);

    static const char* rn[] = {"无","建筑","伐木","采果","打猎","种田","采金","采石"};
    dbg(QString("[村民分配] farmer=%1 role=%2 target=%3 food=%4 wood=%5")
              .arg(sn).arg(rn[role]).arg(t)
              .arg(foodN+(role==ROLE_BUSH || role==ROLE_HUNTER || role==ROLE_FARM))
              .arg(woodN+(role==ROLE_WOOD)));
}

// ====================== 7. 双人猎管理（先杀完附近活体，再共同采集尸体） ======================
// 新村民出生时可能尚未看见瞪羚。后续探路发现猎物后，从普通伐木工中一次补选
// 两名猎人，避免“出生瞬间没看见猎物”永久取消整局双人猎。
static bool recruitHunterPairFromWood(const tagInfo& info)
{
    // 先完成“6名核心食物工”，并等到本应成为两名猎人的后续村民都已出生。
    // 否则开局看见瞪羚时会错误地把初始伐木工提前改成猎人。
    if(g_wave2Handled ||
       (int)g_berryFarmWorkers.size() < TARGET_BERRY_FARM_WORKERS ||
       usrCountLandFarmers(info) < MIN_LAND_FARMERS_FOR_HUNTER_PAIR ||
       g_hunterSN[0] >= 0 || g_hunterSN[1] >= 0) return false;

    vector<const tagFarmer*> candidates;
    for(const auto& farmer : info.farmers)
    {
        if(farmer.FarmerSort != FARMERTYPE_FARMER || farmer.SN == g_builderSN ||
           !snFree(farmer.SN) || farmerHasPendingGatherOrder(farmer.SN) ||
           g_upgradeFoodWorkers.count(farmer.SN)) continue;
        auto role = g_role.find(farmer.SN);
        if(role == g_role.end() || role->second != ROLE_WOOD) continue;
        candidates.push_back(&farmer);
    }
    if(candidates.size() < 2) return false;

    // 优先选择最后出生的两名伐木工，保持“前6名食物工完成后，新村民负责打猎”。
    sort(candidates.begin(), candidates.end(),
         [](const tagFarmer* left, const tagFarmer* right)
         { return left->SN > right->SN; });

    int gazelle = findLiveHuntTarget(info, *candidates[0]);
    if(gazelle < 0) return false;

    const tagFarmer* first = candidates[0];
    const tagFarmer* second = candidates[1];
    g_role[first->SN] = ROLE_HUNTER;
    g_role[second->SN] = ROLE_HUNTER;
    g_farmerGatherTargets.erase(first->SN);
    g_farmerGatherTargets.erase(second->SN);
    g_hunterSN[0] = first->SN;
    g_hunterSN[1] = second->SN;
    g_huntTargetSN = gazelle;
    g_huntKillingLive = true;
    g_hunterPairReady = false;
    g_huntHerdTargets.clear();
    g_huntHerdCaptured = false;
    g_huntIssuedTargetSN = -1;
    g_hunterActive = true;

    // 先让第二人到第一人身边；会合前两人都不会攻击猎物。
    orderMove(second->SN, first->DR, first->UR);
    g_hunterLastMoveFrame = g_aiframe;
    dbg(QString("[打猎补选] 发现猎物%1，伐木工%2和%3转为双人猎")
              .arg(gazelle).arg(first->SN).arg(second->SN));
    return true;
}

static void manageHunters(const tagInfo& info)
{
    recruitHunterPairFromWood(info);
    const tagFarmer* f0 = usrGetFarmer(info, g_hunterSN[0]);
    const tagFarmer* f1 = usrGetFarmer(info, g_hunterSN[1]);

    // 第一名猎人存在而第二名尚未出生时保持原地等待，不惊动猎物。
    if(f0 && !f1)
    {
        g_hunterActive = false;
        return;
    }
    // 一人阵亡时让幸存者成为领队，等待补充出的新村民接替搭档。
    if(!f0 && f1)
    {
        g_hunterSN[0] = f1->SN;
        g_hunterSN[1] = -1;
        g_hunterActive = false;
        dbg(QString("[打猎等待] 原领队失效，猎人%1等待新搭档").arg(f1->SN));
        return;
    }
    if(!f0 && !f1)
    {
        g_hunterSN[0] = g_hunterSN[1] = -1;
        g_hunterActive = false;
        g_huntTargetSN = -1;
        g_huntHerdTargets.clear();
        g_huntHerdCaptured = false;
        g_huntIssuedTargetSN = -1;
        return;
    }

    // 防守期仍继续打猎；只有其中一名猎人被敌军锁定时才暂停双人协同，
    // 让防守状态机优先撤走受攻击者。
    if(g_defenseMode &&
       (enemyTargetingFarmer(info, f0->SN) != -1 ||
        enemyTargetingFarmer(info, f1->SN) != -1))
        return;

    g_hunterActive = true;

    // 第二名猎人先与第一名会合；两人相距两格以内才允许攻击。
    double meetDistance = d2(f0->DR, f0->UR, f1->DR, f1->UR);
    if(meetDistance > HUNTER_MEET_DISTANCE*HUNTER_MEET_DISTANCE*
                      BLOCKSIDELENGTH*BLOCKSIDELENGTH)
    {
        if(f1->NowState == HUMAN_STATE_IDLE && g_aiframe-g_hunterLastMoveFrame >= 100)
        {
            orderMove(f1->SN, f0->DR, f0->UR);
            g_hunterLastMoveFrame = g_aiframe;
        }
        return;
    }
    if(!g_hunterPairReady)
    {
        g_hunterPairReady = true;
        if(!g_huntHerdCaptured) captureHuntHerd(info);
        g_huntTargetSN = findTrackedLiveHuntTarget(info, *f0);
        g_huntIssuedTargetSN = -1;
        dbg(QString("[打猎会合] 猎人%1与%2已会合，首个攻击目标=%3")
                  .arg(f0->SN).arg(f1->SN).arg(g_huntTargetSN));
    }

    // 第一阶段：两人逐只击杀基地附近的活体瞪羚，暂时不采尸体。
    if(g_huntKillingLive)
    {
        const tagResource* target = getRes(info, g_huntTargetSN);
        bool usableLiveTarget = target && target->Type == RESOURCE_GAZELLE &&
                                target->Blood > 0 &&
                                g_huntHerdTargets.count(target->SN) &&
                                usrBlockDist(target->BlockDR, target->BlockUR,
                                             g_tcDR, g_tcUR) <= HUNT_CHASE_LIMIT;
        if(!usableLiveTarget)
        {
            g_huntTargetSN = findTrackedLiveHuntTarget(info, *f0);
            if(g_huntTargetSN < 0)
            {
                g_huntKillingLive = false;
                g_huntIssuedTargetSN = -1;
                dbg(QString("[打猎清场完成] 附近已无活体瞪羚，开始采集尸体"));
            }
        }

        if(g_huntKillingLive)
        {
            // 目标变化时立即覆盖游戏自动采尸体的动作，不等待两名猎人变为空闲。
            bool newTarget = g_huntIssuedTargetSN != g_huntTargetSN;
            bool retryIdle = !newTarget && f0->NowState == HUMAN_STATE_IDLE &&
                             f1->NowState == HUMAN_STATE_IDLE &&
                             f0->WorkObjectSN != g_huntTargetSN &&
                             f1->WorkObjectSN != g_huntTargetSN &&
                             g_aiframe-g_huntLastAttackFrame >= 100;
            if((newTarget || retryIdle) && snFree(f0->SN) && snFree(f1->SN))
            {
                orderAction(info, f0->SN, g_huntTargetSN);
                orderAction(info, f1->SN, g_huntTargetSN);
                g_huntIssuedTargetSN = g_huntTargetSN;
                g_huntLastAttackFrame = g_aiframe;
                dbg(QString("[打猎攻击] 两名猎人共同攻击瞪羚%1，剩余活体=%2")
                          .arg(g_huntTargetSN).arg(g_huntHerdTargets.size()));
            }
            return;
        }
    }

    // 第二阶段：附近没有活体后，两人从最近的尸体开始共同采集。
    const tagResource* carcass = getRes(info, g_huntTargetSN);
    if(!carcass || carcass->Type != RESOURCE_GAZELLE || carcass->Blood > 0 || carcass->Cnt <= 0)
    {
        g_huntTargetSN = findHuntCarcass(info, *f0);
        if(g_huntTargetSN >= 0)
            dbg(QString("[打猎采集] 两名猎人开始采集尸体%1").arg(g_huntTargetSN));
    }

    // 所有尸体都采完后，猎人恢复伐木；六名核心食物工随后一人一田。
    if(g_huntTargetSN < 0)
    {
        int w0 = usrBestGatherResource(info, RESOURCE_TREE, *f0);
        int w1 = usrBestGatherResource(info, RESOURCE_TREE, *f1);
        if(w0 != -1) orderAction(info, f0->SN, w0);
        if(w1 != -1) orderAction(info, f1->SN, w1);
        g_role[f0->SN] = ROLE_WOOD;
        g_role[f1->SN] = ROLE_WOOD;
        g_hunterActive = false;
        g_hunterSN[0] = g_hunterSN[1] = -1;
        g_hunterPairReady = false;
        g_huntHerdTargets.clear();
        g_huntHerdCaptured = false;
        g_huntIssuedTargetSN = -1;
        dbg(QString("[打猎] 瞪羚尸体采完，两名猎人恢复伐木"));
        return;
    }

    if(f0->NowState == HUMAN_STATE_IDLE) orderAction(info, f0->SN, g_huntTargetSN);
    if(f1->NowState == HUMAN_STATE_IDLE) orderAction(info, f1->SN, g_huntTargetSN);
}

// ====================== 8. 建造系统 ======================
// 让指定村民尝试在(cDR,cUR)环内建某建筑；key 用于同类型节流，间隔45帧
static bool tryBuildAt(const tagInfo& info, int builderSN, int bType, int key,
                       int cDR, int cUR, int minR, int maxR)
{
    if(g_aiframe - lastTry(key) < 45) return false;
    int x, y;
    if(!usrFindFlatNear(x, y, info, cDR, cUR, minR, maxR))
    {
        g_lastBuildTry[key] = g_aiframe;   // 没找到地也算一次尝试，避免每帧刷屏
        return false;
    }
    orderBuild(builderSN, bType, x, y);
    g_lastBuildTry[key] = g_aiframe;
    dbg(QString("[建造] 类型%1 @(%2,%3)").arg(bType).arg(x).arg(y));
    return true;
}

// 专职建筑工的建造链（只在他IDLE时派新活；无活时原地待命，保证随叫随到）
static bool militaryProductionNeeded(const tagInfo& info)
{
    // 前两波完全依靠预置箭塔、祭司和转化兵，不主动生产军队。
    // 第二波后补满经济并完成复合弓科技，才为持续造兵预留人口。
    if(!g_wave2Handled || usrCountLandFarmers(info) < TARGET_LAND_FARMERS ||
       !g_compositeTechDone)
        return false;
    for(const auto& b : info.buildings)
    {
        if(b.Percent < 100) continue;
        if(b.Type == BUILDING_RANGE && b.Project != ACT_NULL) return true;
        if(b.Type == BUILDING_RANGE && info.Meat >= COST_COMPOSITE_FOOD &&
           info.Gold >= COST_COMPOSITE_GOLD) return true;
    }
    return false;
}

// 村民采用分阶段上限：升铜前14人，第二波结束前16人，之后补到最终20人。
static int currentLandFarmerTarget(const tagInfo& info)
{
    if(info.civilizationStage < CIVILIZATION_BRONZEAGE)
        return PRE_BRONZE_LAND_FARMERS;
    return g_wave2Handled ? TARGET_LAND_FARMERS : PRE_WAVE2_LAND_FARMERS;
}

static bool populationHouseNeeded(const tagInfo& info)
{
    if(usrCountBuilt(info, BUILDING_HOME) >= MAX_HOUSES) return false;
    if(g_aiframe <= g_houseCapacityRefreshUntil) return false;
    int freePopulation = info.Human_MaxNum - (int)info.Human_Num;
    bool emergencyHouse = info.Human_Num >= info.Human_MaxNum;
    bool economyHouse = usrCountLandFarmers(info) < currentLandFarmerTarget(info) &&
                        freePopulation <= 2;
    bool militaryHouse = militaryProductionNeeded(info) && freePopulation <= 2;
    return emergencyHouse || economyHouse || militaryHouse;
}

static bool hasPendingBuildType(int type)
{
    for(const auto& order : g_pendingBuild)
        if(order.second.type == type) return true;
    for(const auto& build : g_acceptedBuild)
        if(build.second.type == type) return true;
    return false;
}

static const tagBuilding* unfinishedBuilding(const tagInfo& info, int type)
{
    for(const auto& building : info.buildings)
        if(building.Type == type && building.Percent < 100)
            return &building;
    return nullptr;
}

static bool farmerHasPendingGatherOrder(int sn)
{
    for(const auto& order : g_gatherOrders)
        if(order.second == sn) return true;
    return false;
}

static int chooseFoodTarget(const tagInfo& info, const tagFarmer& farmer)
{
    int target = usrBestGatherResource(info, RESOURCE_BUSH, farmer);
    if(target < 0) target = findFreeFarm(info, farmer.SN);
    if(target < 0) target = findHuntCarcass(info, farmer);
    return target;
}

// 市场完成且靶场已经开工后，升级前临时把伐木工转为食物工，形成11名食物工。
// 铜器时代完成后再逐个恢复，避免长期堆木材却攒不到800食物。
static void manageUpgradeFoodWorkers(const tagInfo& info)
{
    bool prerequisitesReady = usrCountBuilt(info, BUILDING_MARKET) >= 1 &&
                              (usrCountBuilt(info, BUILDING_RANGE) >= 1 ||
                               usrCountBuilt(info, BUILDING_STABLE) >= 1);
    bool foodShiftReady = usrCountBuilt(info, BUILDING_MARKET) >= 1 &&
                          (usrHasBuilding(info, BUILDING_RANGE) ||
                           usrHasBuilding(info, BUILDING_STABLE));

    if(info.civilizationStage == CIVILIZATION_TOOLAGE && foodShiftReady &&
       countRole(ROLE_BUSH)+countRole(ROLE_HUNTER)+countRole(ROLE_FARM)
           < TARGET_UPGRADE_FOOD_WORKERS)
    {
        for(const auto& farmer : info.farmers)
        {
            if(farmer.FarmerSort != FARMERTYPE_FARMER || farmer.SN == g_builderSN ||
               !snFree(farmer.SN) || farmerHasPendingGatherOrder(farmer.SN)) continue;
            auto role = g_role.find(farmer.SN);
            if(role == g_role.end() || role->second != ROLE_WOOD) continue;
            int target = chooseFoodTarget(info, farmer);
            if(target < 0) return;

            role->second = ROLE_BUSH;
            g_upgradeFoodWorkers.insert(farmer.SN);
            g_farmerGatherTargets.erase(farmer.SN);
            orderAction(info, farmer.SN, target);
            dbg(QString("[升级食物调度] farmer=%1 wood->food target=%2 foodWorkers=%3")
                      .arg(farmer.SN).arg(target)
                      .arg(countRole(ROLE_BUSH)+countRole(ROLE_HUNTER)+countRole(ROLE_FARM)));
            return;
        }
    }

    // builder建出的农田优先交给临时食物工，摆脱浆果数量限制。
    if(info.civilizationStage == CIVILIZATION_TOOLAGE && prerequisitesReady)
    {
        int farm = findFreeFarm(info);
        if(farm >= 0)
        {
            for(int sn : g_upgradeFoodWorkers)
            {
                const tagFarmer* farmer = usrGetFarmer(info, sn);
                if(!farmer || g_role[sn] == ROLE_FARM || !snFree(sn) ||
                   farmerHasPendingGatherOrder(sn)) continue;
                g_role[sn] = ROLE_FARM;
                g_farmerGatherTargets.erase(sn);
                orderAction(info, sn, farm);
                dbg(QString("[升级农田分配] farmer=%1 farm=%2").arg(sn).arg(farm));
                return;
            }
        }
    }

    if(info.civilizationStage >= CIVILIZATION_BRONZEAGE)
    {
        for(auto it=g_upgradeFoodWorkers.begin(); it!=g_upgradeFoodWorkers.end(); )
        {
            const tagFarmer* farmer = usrGetFarmer(info, *it);
            if(!farmer) { it = g_upgradeFoodWorkers.erase(it); continue; }
            if(!snFree(farmer->SN) || farmerHasPendingGatherOrder(farmer->SN))
            { ++it; continue; }

            int target = usrBestGatherResource(info, RESOURCE_TREE, *farmer);
            if(target < 0) { ++it; continue; }
            g_role[farmer->SN] = ROLE_WOOD;
            g_farmerGatherTargets.erase(farmer->SN);
            orderAction(info, farmer->SN, target);
            dbg(QString("[升级食物恢复] farmer=%1 food->wood target=%2")
                      .arg(farmer->SN).arg(target));
            it = g_upgradeFoodWorkers.erase(it);
            return;
        }
    }
}

// 六名核心食物工按固定顺序工作：浆果 -> 瞪羚尸体 -> 一人一片农田。
// 每帧最多改派一人，避免同一帧争抢同一目标。
static void manageBerryFarmWorkers(const tagInfo& info)
{
    if(g_wave2Handled)
    {
        int assigned = 0;
        for(int sn : g_berryFarmWorkers)
        {
            const tagFarmer* farmer = usrGetFarmer(info, sn);
            if(farmer && usableFarmTarget(info, farmer->WorkObjectSN)) ++assigned;
        }
        static int lastAssigned = -1;
        if(assigned != lastAssigned)
        {
            dbg(QString("[农田食物工] assigned=%1/%2")
                      .arg(assigned).arg(TARGET_BERRY_FARM_WORKERS));
            lastAssigned = assigned;
        }
    }
    const int berryFood = berryFoodNearBase(info);
    for(int sn : g_berryFarmWorkers)
    {
        const tagFarmer* farmer = usrGetFarmer(info, sn);
        if(!farmer || farmer->SN == g_builderSN || !snFree(sn) ||
           (g_defenseMode && enemyTargetingFarmer(info, sn) != -1) ||
           farmerHasPendingGatherOrder(sn)) continue;
        auto role = g_role.find(sn);
        if(role == g_role.end()) continue;
        if(role->second == ROLE_FARM && isCompletedFarm(info, farmer->WorkObjectSN))
            continue;

        const tagResource* current = getRes(info, farmer->WorkObjectSN);
        if(berryFood > 0 && current &&
           current->Type == RESOURCE_BUSH && current->Cnt > 0)
            continue;
        if(berryFood <= 0 && current &&
           current->Type == RESOURCE_GAZELLE && current->Blood <= 0 && current->Cnt > 0)
            continue;

        int target = chooseBerryWorkerFoodTarget(info, *farmer);
        if(target < 0 || farmer->WorkObjectSN == target) continue;

        role->second = isCompletedFarm(info, target) ? ROLE_FARM : ROLE_BUSH;
        g_farmerGatherTargets.erase(sn);
        orderAction(info, sn, target);
        dbg(QString("[核心食物工] farmer=%1 target=%2 role=%3")
                  .arg(sn).arg(target)
                  .arg(role->second == ROLE_FARM ? QString("farm") : QString("food")));
        return;
    }
}

// 第二波结束、20名村民和金矿仓库就绪后，直接从普通伐木工中转出两人采金。
// 不要求伐木工处于IDLE：正常砍树状态也允许被新采金命令替换。
static void manageGoldMiners(const tagInfo& info)
{
    if(!g_wave2Handled || usrCountLandFarmers(info) < TARGET_LAND_FARMERS ||
       !stockNearGold(info) ||
       countRole(ROLE_GOLD) >= TARGET_GOLD_MINER) return;

    for(const auto& farmer : info.farmers)
    {
        if(farmer.FarmerSort != FARMERTYPE_FARMER || farmer.SN == g_builderSN ||
           (g_defenseMode && enemyTargetingFarmer(info, farmer.SN) != -1) ||
           !snFree(farmer.SN) ||
           farmerHasPendingGatherOrder(farmer.SN)) continue;
        auto role = g_role.find(farmer.SN);
        if(role == g_role.end() || role->second != ROLE_WOOD) continue;

        int target = usrBestGatherResource(info, RESOURCE_GOLD, farmer);
        if(target < 0) return;
        role->second = ROLE_GOLD;
        g_farmerGatherTargets.erase(farmer.SN);
        orderAction(info, farmer.SN, target);
        dbg(QString("[采金转换] farmer=%1 wood->gold target=%2")
                  .arg(farmer.SN).arg(target));
        return;
    }
}

// 前两波仍暂停建设保塔；之后只在建筑工附近有敌军或需要可执行的修塔时暂停。
static bool pauseEconomicBuilds(const tagInfo& info)
{
    if(!g_defenseMode) return false;
    if(!g_wave2Handled) return true;
    const tagFarmer* builder = usrGetFarmer(info, g_builderSN);
    if(!builder || enemyTargetingFarmer(info, g_builderSN) != -1) return true;
    const double radius = DEFENSE_SAFE_FARMER_BLOCKS*BLOCKSIDELENGTH;
    for(const auto& enemy : info.enemy_armies)
        if(enemy.Blood > 0 &&
           (d2(enemy.DR, enemy.UR, builder->DR, builder->UR) <= radius*radius ||
            d2(enemy.DR, enemy.UR, g_tcDR*BLOCKSIDELENGTH,
               g_tcUR*BLOCKSIDELENGTH) <= radius*radius)) return true;
    const tagBuilding* tower = getBuilding(info, g_defTowerSN);
    return tower && tower->Blood > 0 && tower->Blood < tower->MaxBlood && info.Stone > 0 &&
           !hasPendingBuildType(BUILDING_HOME) && !unfinishedBuilding(info, BUILDING_HOME);
}

static void builderQueue(const tagInfo& info, bool emergencyOnly)
{
    if(g_builderSN == -1) return;
    const tagFarmer* bf = usrGetFarmer(info, g_builderSN);
    if(!bf) { g_builderSN = -1; return; }

    int camps   = usrCountBuilt(info, BUILDING_ARMYCAMP);
    int markets = usrCountBuilt(info, BUILDING_MARKET);
    int ranges  = usrCountBuilt(info, BUILDING_RANGE);
    bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;
    bool upgradePrerequisites = markets >= 1 && ranges >= 1;

    int landFarmers = usrCountLandFarmers(info);
    int freePopulation = info.Human_MaxNum - (int)info.Human_Num;
    int farmerTarget = currentLandFarmerTarget(info);
    bool emergencyHouse = info.Human_Num >= info.Human_MaxNum;
    bool economyHouse = landFarmers < farmerTarget && freePopulation <= 2;
    bool militaryHouse = militaryProductionNeeded(info) && freePopulation <= 2;
    bool needHouse = populationHouseNeeded(info);

    // 只在房屋需求变化时记录一次。info.Human_Num 包含刚转化的单位，所以下一帧会立即重算。
    static int lastHouseReason = -1;
    int houseReason = (emergencyHouse ? 1 : 0) |
                      (economyHouse ? 2 : 0) |
                      (militaryHouse ? 4 : 0);
    if(houseReason != lastHouseReason)
    {
        dbg(QString("[人口房检查] population=%1/%2 farmers=%3/%4 emergency=%5 economy=%6 military=%7")
                  .arg(info.Human_Num, 0, 'f', 1).arg(info.Human_MaxNum)
                  .arg(landFarmers).arg(farmerTarget)
                  .arg(emergencyHouse ? 1 : 0)
                  .arg(economyHouse ? 1 : 0)
                  .arg(militaryHouse ? 1 : 0));
        lastHouseReason = houseReason;
    }

    // 防守期间人口超限只会暂停生产，不会损失现有单位；此时建筑工必须留在
    // 箭塔旁持续维修。房屋需求和在建进度都保留，防守结束后自动恢复施工。
    static bool combatHouseDeferredLogged = false;
    if(emergencyOnly)
    {
        bool hasHouseTask = needHouse || hasPendingBuildType(BUILDING_HOME) ||
                            unfinishedBuilding(info, BUILDING_HOME) != nullptr;
        if(hasHouseTask && !combatHouseDeferredLogged)
        {
            const tagBuilding* tower = getBuilding(info, g_defTowerSN);
            dbg(QString("[防守房屋延后] population=%1/%2 tower=%3 blood=%4/%5")
                      .arg(info.Human_Num,0,'f',1).arg(info.Human_MaxNum)
                      .arg(tower ? tower->SN : -1)
                      .arg(tower ? tower->Blood : 0)
                      .arg(tower ? tower->MaxBlood : 0));
            combatHouseDeferredLogged = true;
        }
        return;
    }
    combatHouseDeferredLogged = false;

    if(g_wave2Handled)
    {
        if(g_farmResumeOrder >= 0) return;
        // 游戏会让完工农田的建筑工自动种田，先释放他，下一帧交给核心食物工。
        if(usableFarmTarget(info, bf->WorkObjectSN))
        {
            orderMove(bf->SN, bf->DR, bf->UR);
            dbg(QString("[农田交接] builder=%1 farm=%2，建筑工退出种田")
                      .arg(bf->SN).arg(bf->WorkObjectSN));
            return;
        }
    }

    // 房屋刚完成时等待Human_MaxNum刷新；这几帧也不启动其他建筑。
    if(g_aiframe <= g_houseCapacityRefreshUntil) return;

    // 已下单或已出现的房屋必须先完成；同一时间绝不再提交第二座房屋。
    if(hasPendingBuildType(BUILDING_HOME)) return;
    if(const tagBuilding* house = unfinishedBuilding(info, BUILDING_HOME))
    {
        if(bf->WorkObjectSN == house->SN) return;
        if(bf->NowState != HUMAN_STATE_IDLE && !emergencyHouse) return;
        if(g_defenseMode && enemyTargetingFarmer(info, g_builderSN) != -1) return;
        orderBuilderWork(g_builderSN, house->SN);
        dbg(QString("[人口房续建] builder=%1 house=%2 percent=%3")
                  .arg(g_builderSN).arg(house->SN).arg(house->Percent));
        return;
    }

    // 紧急、经济和军队三类人口房都优先于农田与其他新建筑。
    if(needHouse)
    {
        // 普通扩容等待builder空闲；人口已锁死时可中断原来的非房屋工作。
        if(bf->NowState != HUMAN_STATE_IDLE && !emergencyHouse) return;
        if(g_defenseMode && enemyTargetingFarmer(info, g_builderSN) != -1) return;
        if(info.Wood >= COST_HOUSE_WOOD &&
           tryBuildAt(info, g_builderSN, BUILDING_HOME, BUILDING_HOME,
                      g_tcDR, g_tcUR, 5, 16))
        {
            const QString reason = emergencyHouse ? QString("emergency")
                                   : economyHouse ? QString("economy")
                                                  : QString("military");
            dbg(QString("[人口房下单] reason=%1 population=%2/%3 farmers=%4/%5")
                      .arg(reason).arg(info.Human_Num, 0, 'f', 1)
                      .arg(info.Human_MaxNum).arg(landFarmers).arg(farmerTarget));
        }
        return;
    }

    if(bf->NowState != HUMAN_STATE_IDLE) return;

    // 防守撤退等情况可能中断前置建筑施工。回去续建，不能另起一座导致升铜延误。
    for(const auto& building : info.buildings)
    {
        bool upgradePrerequisite = building.Type == BUILDING_ARMYCAMP ||
                                   building.Type == BUILDING_MARKET ||
                                   building.Type == BUILDING_RANGE;
        if(upgradePrerequisite && building.Percent > 0 && building.Percent < 100)
        {
            orderBuilderWork(g_builderSN, building.SN);
            dbg(QString("[前置续建] builder=%1 type=%2 sn=%3 percent=%4")
                      .arg(g_builderSN).arg(building.Type).arg(building.SN)
                      .arg(building.Percent));
            return;
        }
    }

    // 第一波结束后优先把预置箭塔修满，再用它迎接第二波。
    // HumanAction指向受损友方建筑时，游戏内核会自动进入修理状态。
    if(g_wave1Handled && !g_wave2Handled && g_defTowerSN != -1)
    {
        const tagBuilding* tower = getBuilding(info, g_defTowerSN);
        if(tower && tower->Blood >= tower->MaxBlood && g_towerRepairStarted)
        {
            g_towerRepairStarted = false;
            dbg(QString("[箭塔维修完成] tower=%1 blood=%2/%3")
                      .arg(tower->SN).arg(tower->Blood).arg(tower->MaxBlood));
        }
        if(tower && tower->Percent >= 100 && tower->Blood > 0 && tower->Blood < tower->MaxBlood)
        {
            static int lastTowerRepairOrder = -1000;
            if(g_aiframe-lastTowerRepairOrder >= 100)
            {
                orderBuilderWork(g_builderSN, tower->SN);
                lastTowerRepairOrder = g_aiframe;
                if(!g_towerRepairStarted)
                    dbg(QString("[箭塔维修] builder=%1 tower=%2 blood=%3/%4")
                              .arg(g_builderSN).arg(tower->SN)
                              .arg(tower->Blood).arg(tower->MaxBlood));
                g_towerRepairStarted = true;
            }
            return;
        }
    }

    // (1) 兵营（靶场前置；前两波不在这里生产军队）
    if(camps < 1 && info.Wood >= COST_CAMP_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_ARMYCAMP, BUILDING_ARMYCAMP,
                   g_tcDR, g_tcUR, 5, 16);
        return;
    }
    // (2) 市场（升铜前置之一）
    if(markets < 1 && info.Wood >= COST_MARKET_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_MARKET, BUILDING_MARKET,
                   g_tcDR, g_tcUR, 5, 16);
        return;
    }
    // (3) 靶场（升铜前置之二，150木，需兵营）
    if(camps >= 1 && ranges < 1 && info.Wood >= COST_RANGE_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_RANGE, BUILDING_RANGE,
                   g_tcDR, g_tcUR, 5, 16);
        return;
    }
    // 升级前置齐全后，最多补2片农田提供稳定食物；此时不再补升级用不到的房屋。
    if(!bronze && upgradePrerequisites)
    {
        int farms = 0;
        for(const auto& building : info.buildings)
            if(building.Type == BUILDING_FARM && building.Percent > 0) ++farms;
        if(farms < 2 && info.Wood >= COST_FARM_WOOD)
        {
            tryBuildAt(info, g_builderSN, BUILDING_FARM, BUILDING_FARM,
                       g_tcDR, g_tcUR, 4, 14);
            return;
        }
    }
    // 前置建筑完成后为800食物升级留资源，铜器时代前不再插入其他建筑。
    if(!bronze) return;

    // 第二波前完成16名村民后保存资源，不再增加科技、农田或建筑。
    if(!g_wave2Handled) return;

    // 保留原有采金、科技与第二靶场的建设窗口；食物工断粮时允许提前补田。
    // 耗尽或明确无法续建的田不计数，仍逐片补建并保留350木材。
    bool foodFarmNeeded = false;
    for(int sn : g_berryFarmWorkers)
    {
        const tagFarmer* worker = usrGetFarmer(info, sn);
        if(!worker || usableFarmTarget(info, worker->WorkObjectSN)) continue;
        const tagResource* food = getRes(info, worker->WorkObjectSN);
        if(food && food->Cnt > 0 && (food->Type == RESOURCE_BUSH ||
           (food->Type == RESOURCE_GAZELLE && food->Blood <= 0))) continue;
        if(chooseBerryWorkerFoodTarget(info, *worker) < 0) { foodFarmNeeded = true; break; }
    }
    bool foodFarmWindow = landFarmers < TARGET_LAND_FARMERS ||
                          (g_goldStockBuilt && !g_compositeTechDone) || ranges >= 2 ||
                          foodFarmNeeded;
    if(foodFarmWindow && berryFoodNearBase(info) <= 0)
    {
        int farms = 0;
        const tagBuilding* unfinishedFarm = nullptr;
        for(const auto& building : info.buildings)
            if(building.Type == BUILDING_FARM && building.Percent > 0 &&
               building.Cnt > 0 && !g_failedFarms.count(building.SN))
            {
                ++farms;
                if(building.Percent < 100 && !unfinishedFarm) unfinishedFarm = &building;
            }

        if(hasPendingBuildType(BUILDING_FARM)) return;
        if(unfinishedFarm)
        {
            resumeFarm(*bf, *unfinishedFarm);
            return;
        }
        if(farms < TARGET_BERRY_FARM_WORKERS &&
           info.Wood >= POST_WAVE2_FARM_WOOD_RESERVE + COST_FARM_WOOD &&
           tryBuildAt(info, g_builderSN, BUILDING_FARM, BUILDING_FARM,
                      g_tcDR, g_tcUR, 4, 14))
        {
            dbg(QString("[浆果转农田准备] berry=0 farms=%1/%2 woodReserve=%3")
                      .arg(farms+1).arg(TARGET_BERRY_FARM_WORKERS)
                      .arg(POST_WAVE2_FARM_WOOD_RESERVE));
            return;
        }
    }

    // 第二波结束后先补足20名村民，经济建筑从下一阶段开始。
    if(landFarmers < TARGET_LAND_FARMERS) return;

    // (1) 金矿旁仓库必须先建成。含在建仓库时只等待，不重复另建。
    g_goldStockBuilt = stockNearGold(info);
    if(!g_goldStockBuilt)
    {
        double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
        int goldSN = usrNearestResource(info, RESOURCE_GOLD, tcx, tcy, false);
        const tagResource* gold = goldSN>=0 ? getRes(info, goldSN) : nullptr;
        if(!gold) return;

        bool stockInProgress = false;
        for(const auto& building : info.buildings)
        {
            if(building.Type == BUILDING_STOCK && building.Percent < 100 &&
               usrBlockDist(building.BlockDR, building.BlockUR,
                            gold->BlockDR, gold->BlockUR) <= 8.0)
            {
                stockInProgress = true;
                if(bf->WorkObjectSN != building.SN)
                    orderBuilderWork(g_builderSN, building.SN);
                break;
            }
        }
        if(!stockInProgress && info.Wood >= COST_STOCK_WOOD)
        {
            tryBuildAt(info, g_builderSN, BUILDING_STOCK, KEY_STOCK_GOLD,
                       gold->BlockDR, gold->BlockUR, 2, 6);
        }
        return;
    }

    // (2) 两名采金工由manageGoldMiners逐帧强制改派；未到位前保持等待。
    if(countRole(ROLE_GOLD) < TARGET_GOLD_MINER) return;

    // (3)(4) 市场伐木科技与复合弓科技由techAndProduction依次完成。
    if(!g_woodTechDone || !g_compositeTechDone) return;

    // (5) 两项科技完成后补第二座靶场。
    if(ranges < 2)
    {
        if(info.Wood >= COST_RANGE_WOOD)
            tryBuildAt(info, g_builderSN, BUILDING_RANGE, BUILDING_RANGE,
                       g_tcDR, g_tcUR, 5, 16);
        return;
    }

}

// ====================== 9. 科技 / 升时代 / 造兵 ======================
static void processFarmerProduction(const tagInfo& info)
{
    int farmers = usrCountLandFarmers(info);
    if(farmers >= TARGET_LAND_FARMERS && !g_farmerCapLogged)
    {
        g_farmerCapLogged = true;
        dbg(QString("[村民上限] 陆地村民达到%1，停止生产").arg(farmers));
    }
    if(g_farmerOrderId < 0) return;
    auto result = info.ins_ret.find(g_farmerOrderId);
    bool failed = result != info.ins_ret.end() && result->second != ACTION_SUCCESS;
    bool completed = farmers > g_farmerOrderBaseCount;
    bool timedOut = g_aiframe-g_farmerOrderFrame >= SEC(30);
    if(failed || completed || timedOut)
    {
        if(failed) dbg(QString("[村民生产恢复] order=%1 code=%2")
                          .arg(g_farmerOrderId).arg(result->second));
        g_farmerOrderId = -1;
        g_farmerOrderBaseCount = -1;
        g_farmerOrderFrame = -1;
    }
}

static void techAndProduction(const tagInfo& info)
{
    const int landFarmers = usrCountLandFarmers(info);
    const int farmerTarget = currentLandFarmerTarget(info);
    bool militaryQueuedThisFrame = false;
    static int lastFarmerTarget = -1;
    if(farmerTarget != lastFarmerTarget)
    {
        dbg(QString("[村民阶段目标] current=%1 farmers=%2 wave2Handled=%3")
                  .arg(farmerTarget).arg(landFarmers).arg(g_wave2Handled ? 1 : 0));
        lastFarmerTarget = farmerTarget;
    }
    for(const auto& b : info.buildings)
    {
        if(b.Percent < 100 || b.Project != ACT_NULL) continue;

        // —— 市镇中心 ——
        if(b.Type == BUILDING_CENTER)
        {
            // 升铜器：800食 + 市场 + (靶场或马厩) 建成。
            // 升级必须优先于造村民，否则TC会一直排队造人导致升级点不下去。
            bool pre2 = usrCountBuilt(info, BUILDING_MARKET) >= 1
                        && (usrCountBuilt(info, BUILDING_RANGE) >= 1
                            || usrCountBuilt(info, BUILDING_STABLE) >= 1);
            if(info.civilizationStage == CIVILIZATION_TOOLAGE &&
               info.Meat >= COST_BRONZE_FOOD && !pre2)
            {
                static int lastMissingMask = -1;
                int missingMask = (usrCountBuilt(info, BUILDING_MARKET) < 1 ? 1 : 0) |
                                  (usrCountBuilt(info, BUILDING_RANGE) < 1 &&
                                   usrCountBuilt(info, BUILDING_STABLE) < 1 ? 2 : 0);
                if(missingMask != lastMissingMask)
                {
                    dbg(QString("[升级等待] 食物=%1 市场完成=%2 靶场或马厩完成=%3")
                              .arg(info.Meat).arg((missingMask & 1) == 0)
                              .arg((missingMask & 2) == 0));
                    lastMissingMask = missingMask;
                }
            }
            if(info.civilizationStage == CIVILIZATION_TOOLAGE
               && info.Meat >= COST_BRONZE_FOOD && pre2)
            {
                orderBldAction(b.SN, BUILDING_CENTER_UPGRADE);
                dbg(QString("[升级] 点击升铜器时代（800食物/60秒）"));
            }
            // 未达到当前阶段目标就补村民；死亡后也按同一目标补足。
            else if(landFarmers < farmerTarget &&
                    g_farmerOrderId < 0 &&
                    info.Human_Num < info.Human_MaxNum && info.Meat >= COST_FARMER_FOOD &&
                    snFree(b.SN))
            {
                g_farmerOrderId = g_ai->BuildingAction(b.SN, BUILDING_CENTER_CREATEFARMER);
                g_farmerOrderBaseCount = landFarmers;
                g_farmerOrderFrame = g_aiframe;
                markSN(b.SN);
            }
        }
        // —— 谷仓：箭塔科技（50食/10秒），早点研发，食物留10缓冲 ——
        else if(b.Type == BUILDING_GRANARY)
        {
            if(!g_towerTechIssued && info.Meat >= COST_TOWERTECH_FOOD + 10)
            {
                orderBldAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                g_towerTechIssued = true;
                dbg(QString("[科技] 谷仓研发箭塔科技"));
            }
        }
        // —— 市场：第二波后、20村民和2名采金工到位后才研究伐木科技 ——
        else if(b.Type == BUILDING_MARKET)
        {
            if(g_wave2Handled && landFarmers >= TARGET_LAND_FARMERS &&
               countRole(ROLE_GOLD) >= TARGET_GOLD_MINER
               && !g_woodTechIssued && info.Meat >= COST_TECH_WOOD_F && info.Wood >= COST_TECH_WOOD_W)
            {
                orderBldAction(b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                g_woodTechIssued = true;
                dbg(QString("[科技] 市场研发伐木科技"));
            }
        }
        // —— 靶场：前两波不造兵；第二波后按科技顺序研发并生产复合弓兵 ——
        else if(b.Type == BUILDING_RANGE)
        {
            if(g_wave2Handled && landFarmers >= TARGET_LAND_FARMERS &&
                    countRole(ROLE_GOLD) >= TARGET_GOLD_MINER && g_woodTechDone &&
                    !g_compositeIssued &&
                    info.Meat >= COST_TECH_COMP_F && info.Wood >= COST_TECH_COMP_W)
            {
                orderBldAction(b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
                g_compositeIssued = true;
                dbg(QString("[科技] 靶场研发复合弓科技"));
            }
            else if(g_wave2Handled && landFarmers >= TARGET_LAND_FARMERS &&
                    g_compositeTechDone && !militaryQueuedThisFrame &&
                    info.Human_Num < info.Human_MaxNum &&
                    info.Meat >= COST_COMPOSITE_FOOD && info.Gold >= COST_COMPOSITE_GOLD)
            {
                orderBldAction(b.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
                militaryQueuedThisFrame = true;
                dbg(QString("[复合弓生产] range=%1").arg(b.SN));
            }
        }
    }

    // 科技完成需要先看到建筑忙碌，再看到它恢复空闲，避免把“已下单”误当成“已完成”。
    if(g_woodTechIssued && !g_woodTechDone)
    {
        for(const auto& building : info.buildings)
        {
            if(building.Type != BUILDING_MARKET || building.Percent < 100) continue;
            if(building.Project != ACT_NULL) g_woodTechBusy = true;
            else if(g_woodTechBusy)
            {
                g_woodTechDone = true;
                dbg(QString("[科技完成] 市场伐木科技"));
            }
        }
    }

    if(g_compositeIssued && !g_compositeTechDone)
    {
        for(const auto& building : info.buildings)
        {
            if(building.Type != BUILDING_RANGE || building.Percent < 100) continue;
            if(building.Project != ACT_NULL) g_compositeTechBusy = true;
            else if(g_compositeTechBusy)
            {
                g_compositeTechDone = true;
                dbg(QString("[科技完成] 复合弓科技"));
            }
        }
    }

    // 跟踪箭塔科技是否完成（下单后见过Project忙碌，再回到ACT_NULL即完成）
    if(g_towerTechIssued && !g_towerTechDone)
    {
        for(const auto& b : info.buildings)
        {
            if(b.Type != BUILDING_GRANARY) continue;
            if(b.Project != ACT_NULL) g_towerTechBusy = true;
            else if(g_towerTechBusy)
            {
                g_towerTechDone = true;
                dbg(QString("[科技完成] 箭塔科技"));
            }
        }
    }
}

// 第三波提前拦截进入主塔保护范围的投石车，不等它开始轰塔；离开范围则撤回。
// 箭塔被毁后仍以已保存的塔坐标判断范围，直到附近投石车被消灭或转化。
static const tagArmy* findTowerSiege(const tagInfo& info, double fromDR, double fromUR,
                                   double rangeBlocks, int preferredSN = -1)
{
    if(!g_defenseMode || !g_wave2Handled || g_activeDefenseWave < 3) return nullptr;
    if(g_defTowerDR < 0 || g_defTowerUR < 0) return nullptr;
    const double radius = SIEGE_INTERCEPT_RADIUS_BLOCKS * BLOCKSIDELENGTH;
    const double range = rangeBlocks * BLOCKSIDELENGTH;
    const tagArmy* best = nullptr;
    double bestDistance = 1e30;
    for(const auto& enemy : info.enemy_armies)
    {
        if(enemy.Sort != AT_STONE_THROWER || enemy.Blood <= 0 ||
           (g_priestSN != -1 && enemy.WorkObjectSN == g_priestSN && enemy.SN != preferredSN) ||
           d2(enemy.DR, enemy.UR, g_defTowerDR*BLOCKSIDELENGTH,
              g_defTowerUR*BLOCKSIDELENGTH) > radius*radius) continue;
        double distance = d2(fromDR, fromUR, enemy.DR, enemy.UR);
        if(distance > range*range) continue;
        if(enemy.SN == preferredSN) return &enemy;
        if(distance < bestDistance) { best = &enemy; bestDistance = distance; }
    }
    return best;
}

// ====================== 10. 祭司：前期有限探路，后期塔后转换 ======================
static void managePriest(const tagInfo& info)
{
    g_priestSN = -1;
    for(const auto& a : info.armies)
        if(a.Sort == AT_PRIEST) { g_priestSN = a.SN; break; }
    if(g_priestSN == -1)
    {
        g_priestDangerRetreat = false;
        g_priestDangerAttacker = -1;
        g_priestDangerSafeFrames = 0;
        return;
    }
    const tagArmy* p = usrGetArmy(info, g_priestSN);
    if(!p) return;

    refreshPriestPoint();
    double pd = d2(p->DR, p->UR, g_priestPointDR, g_priestPointUR);

    // 祭司附近14格是否出现敌兵（任何时期，出现就立刻撤回安全点，保命第一）
    int nearbyEnemy = -1; double ne = 14.0*14.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH;
    for(const auto& e : info.enemy_armies)
    {
        double dd = d2(p->DR, p->UR, e.DR, e.UR);
        if(dd < ne) { ne = dd; nearbyEnemy = e.SN; }
    }

    if(g_defenseMode)
    {
        // 敌军已经锁定祭司时，避险优先级高于站位和转换。
        // 被锁定时从塔后4格收缩到塔后2格，让箭塔继续挡在祭司与敌军之间，
        // 同时迫使远程追兵进入箭塔射程；不能再向后跑到塔外8格。
        const tagArmy* attacker = nullptr;
        double attackerDistance = 1e30;
        // 当前追兵仍在锁定祭司时保持同一个避险方向，避免多名敌人同时出现时
        // 每帧在不同方向之间折返。只有当前追兵解除锁定后才选择下一名。
        for(const auto& enemy : info.enemy_armies)
            if(enemy.SN == g_priestDangerAttacker &&
               enemy.WorkObjectSN == g_priestSN)
            {
                attacker = &enemy;
                break;
            }
        if(!attacker)
        {
            int bestPriority = 99;
            for(const auto& enemy : info.enemy_armies)
            {
                if(enemy.WorkObjectSN != g_priestSN) continue;
                double distance = d2(p->DR, p->UR, enemy.DR, enemy.UR);
                int priority = isDangerousRangedEnemy(enemy.Sort) ? 0 : 1;
                if(priority < bestPriority ||
                   (priority == bestPriority && distance < attackerDistance))
                {
                    attacker = &enemy;
                    bestPriority = priority;
                    attackerDistance = distance;
                }
            }
        }

        // 只要敌军仍锁定祭司，就暂停转换并贴到塔后2格避险。
        // 等箭塔把追兵拉走后，再从这个位置继续转换箭塔当前目标。
        bool mustRetreat = attacker != nullptr;
        if(mustRetreat)
        {
            // 每名追兵只计算一次撤退点。站到箭塔相对该追兵的背面2格，
            // 使射程同为7格的战车弓兵停火时处于箭塔射程内。
            if(!g_priestDangerRetreat || g_priestDangerAttacker != attacker->SN)
            {
                double tx = (g_defTowerSN != -1 ? g_defTowerDR : g_tcDR) * BLOCKSIDELENGTH;
                double ty = (g_defTowerSN != -1 ? g_defTowerUR : g_tcUR) * BLOCKSIDELENGTH;
                double dx = tx-attacker->DR, dy = ty-attacker->UR;
                double length = sqrt(dx*dx+dy*dy);
                if(length < 0.5)
                {
                    dx = g_priestPointDR-tx;
                    dy = g_priestPointUR-ty;
                    length = sqrt(dx*dx+dy*dy);
                }
                if(length < 0.5) { dx = 1.0; dy = 0.0; length = 1.0; }

                g_priestRetreatDR = tx + dx/length*
                    PRIEST_EMERGENCY_BEHIND_TOWER_BLOCKS*BLOCKSIDELENGTH;
                g_priestRetreatUR = ty + dy/length*
                    PRIEST_EMERGENCY_BEHIND_TOWER_BLOCKS*BLOCKSIDELENGTH;
                double maxDR = ((int)info.theMap->size()-1)*BLOCKSIDELENGTH;
                double maxUR = ((int)(*info.theMap)[0].size()-1)*BLOCKSIDELENGTH;
                g_priestRetreatDR = max(0.5*BLOCKSIDELENGTH,
                                        min(maxDR, g_priestRetreatDR));
                g_priestRetreatUR = max(0.5*BLOCKSIDELENGTH,
                                        min(maxUR, g_priestRetreatUR));
                bool firstDanger = !g_priestDangerRetreat;
                g_priestDangerRetreat = true;
                g_priestDangerAttacker = attacker->SN;
                g_priestLastMoveFrame = -1000;
                dbg(QString(firstDanger ?
                            "[祭司遇险] attacker=%1 sort=%2 blood=%3/%4 retreat=(%5,%6)" :
                            "[祭司追兵交接] nextAttacker=%1 sort=%2 blood=%3/%4 retreat=(%5,%6)")
                          .arg(attacker->SN).arg(attacker->Sort)
                          .arg(p->Blood).arg(p->MaxBlood)
                          .arg(g_priestRetreatDR/BLOCKSIDELENGTH,0,'f',1)
                          .arg(g_priestRetreatUR/BLOCKSIDELENGTH,0,'f',1));
            }
            g_priestDangerSafeFrames = 0;
            if(d2(p->DR, p->UR, g_priestRetreatDR, g_priestRetreatUR) >
               1.0*1.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH &&
               g_aiframe-g_priestLastMoveFrame >= 75)
            {
                // 移动命令可以中断正在进行的转换，优先保住祭司。
                orderMove(g_priestSN, g_priestRetreatDR, g_priestRetreatUR);
                g_priestLastMoveFrame = g_aiframe;
            }
            return;
        }

        if(g_priestDangerRetreat)
        {
            ++g_priestDangerSafeFrames;
            if(g_priestDangerSafeFrames == 1)
                dbg(QString("[祭司追兵脱离] attacker=%1 已停止锁定，防守期间保持塔后2格")
                          .arg(g_priestDangerAttacker));

            if(d2(p->DR, p->UR, g_priestRetreatDR, g_priestRetreatUR) >
               1.0*1.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
            {
                if(g_aiframe-g_priestLastMoveFrame >= 75)
                {
                    orderMove(g_priestSN, g_priestRetreatDR, g_priestRetreatUR);
                    g_priestLastMoveFrame = g_aiframe;
                }
                return;
            }
        }

        // —— 防守期：先准确站到塔后4格，再开始转换 ——
        // 若边移动边继续向下执行，移动命令会占用本帧指令，随后产生“记录了转换、
        // 实际没有发出转换”的假日志，因此未到位时必须在这里结束本帧处理。
        // 一旦进入紧急避险，本轮防守结束前始终保持塔后2格并在此转换；
        // 防守状态真正解除后，才恢复到常规塔后4格。
        if(!g_priestDangerRetreat &&
           pd > 1.0*1.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
        {
            if(p->NowState != HUMAN_STATE_ATTACKING &&
               g_aiframe - g_priestLastMoveFrame >= 200)
            {
                orderMove(g_priestSN, g_priestPointDR, g_priestPointUR);
                g_priestLastMoveFrame = g_aiframe;
            }
            return;
        }

        // 第三波的例外：已站稳、无人锁定、冷却结束时，优先转换射程内的投石车。
        // 不追出阵地，也不打断已经开始的转换；上面的遇险撤退仍优先。
        const tagArmy* siege = findTowerSiege(info, p->DR, p->UR, 12.0,
                                            g_priestConvertTarget);
        if(siege && p->NowState != HUMAN_STATE_ATTACKING &&
           p->ConvertCooldown == 0 && snFree(g_priestSN))
        {
            orderAction(info, g_priestSN, siege->SN);
            g_priestConvertTarget = siege->SN;
            dbg(QString("[祭司优先转投石车] priest=%1 target=%2 distance=%3")
                      .arg(g_priestSN).arg(siege->SN)
                      .arg(sqrt(d2(p->DR, p->UR, siege->DR, siege->UR))/BLOCKSIDELENGTH,0,'f',1));
            return;
        }

        // 箭塔本帧刚切换目标时，info中的Project仍是旧值；等下一帧再转换，
        // 避免箭塔和祭司分别处理两个目标。
        if(g_towerLastAttackFrame == g_aiframe) return;

        // 常规转换跟随箭塔的 Project，避免另选敌人引来追击。
        // 第三波射程内投石车的优先处理已在上面的分支完成。
        int towerTarget = -1;
        if(g_defTowerSN != -1)
        {
            const tagBuilding* tw = getBuilding(info, g_defTowerSN);
            if(tw && tw->Project != ACT_NULL)
            {
                for(const auto& e : info.enemy_armies)
                    if(e.SN == tw->Project) { towerTarget = e.SN; break; }
            }
        }

        // 正在转换中（ATTACKING）不重复下令，等它念完
        if(p->NowState == HUMAN_STATE_ATTACKING) return;
        // 冷却没好不能转
        if(p->ConvertCooldown != 0) return;
        // 必须等箭塔拥有一个仍然有效的攻击目标。
        if(towerTarget == -1) return;

        // 箭塔射程与塔后4格站位保证大多数目标会进入祭司12格转换射程；
        // 目标尚未进入射程时继续等待，不改转其他敌人。
        double range12 = 12.0*12.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH;
        const tagArmy* targetArmy = nullptr;
        for(const auto& e : info.enemy_armies)
            if(e.SN == towerTarget) { targetArmy = &e; break; }
        if(targetArmy && snFree(g_priestSN) &&
           d2(p->DR, p->UR, targetArmy->DR, targetArmy->UR) <= range12)
        {
            orderAction(info, g_priestSN, towerTarget);
            g_priestConvertTarget = towerTarget;
            dbg(QString("[祭司] 箭塔目标=%1，祭司开始转换").arg(towerTarget));
        }
        return;
    }

    g_priestDangerRetreat = false;
    g_priestDangerAttacker = -1;
    g_priestDangerSafeFrames = 0;

    // —— 非防守期 ——
    // 第二波以后祭司不再探路，固定留在塔后4格保存血量和转换能力。
    if(g_wave2Handled)
    {
        static bool priestHoldLogged = false;
        if(!priestHoldLogged)
        {
            dbg(QString("[祭司留守] 第二波结束，停止探路并驻守箭塔后方"));
            priestHoldLogged = true;
        }
        if(pd > 1.0*1.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH &&
           g_aiframe-g_priestLastMoveFrame >= 200)
        {
            orderMove(g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_aiframe;
        }
        return;
    }

    // 3:20~第一波结束回家；第一波后重新探路；8:00起等待第二波。
    // 任何时候附近撞见敌人都立即回家。
    bool waitingWave1 = g_aiframe >= FRAME_PRIEST_RETURN && !g_wave1Handled;
    bool waitingWave2 = g_aiframe >= SEC(480) && g_wave1Handled && !g_wave2Handled;
    bool goHome = nearbyEnemy != -1 || waitingWave1 || waitingWave2;
    if(goHome)
    {
        if(pd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH
           && g_aiframe - g_priestLastMoveFrame >= 300)
        {
            orderMove(g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_aiframe;
        }
        return;
    }

    // 沿TC周围8个方向轮流探路；通过阶段半径逐步扩大已探索区域。
    static const int dx8[8] = {0, 1, 1, 1, 0,-1,-1,-1};
    static const int dy8[8] = {1, 1, 0,-1,-1,-1, 0, 1};
    double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
    double scoutR = g_wave1Handled ? 22.0 : 15.0;
    static int lastScoutRadius = -1;
    if((int)scoutR != lastScoutRadius)
    {
        lastScoutRadius = (int)scoutR;
        dbg(QString("[祭司探路] radius=%1 wave1=%2 wave2=%3")
                  .arg(lastScoutRadius).arg(g_wave1Handled ? 1 : 0)
                  .arg(g_wave2Handled ? 1 : 0));
    }
    int k = g_priestScoutIdx % 8;
    double tx = tcx + dx8[k]*scoutR*BLOCKSIDELENGTH;
    double ty = tcy + dy8[k]*scoutR*BLOCKSIDELENGTH;
    // 已到当前探路点（4格内）就先切下一个方向
    if(d2(p->DR,p->UR,tx,ty) < 4.0*4.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
    {
        g_priestScoutIdx++;
        k = g_priestScoutIdx % 8;
        tx = tcx + dx8[k]*scoutR*BLOCKSIDELENGTH;
        ty = tcy + dy8[k]*scoutR*BLOCKSIDELENGTH;
    }
    // 移动指令400帧节流；每次下令后推进方向，
    // 既解决"一直打印移动同一位置"，也防止目标点在海里时卡死
    if(g_aiframe - g_priestLastMoveFrame >= 400)
    {
        orderMove(g_priestSN, tx, ty);
        g_priestLastMoveFrame = g_aiframe;
        g_priestScoutIdx++;
    }
}

// ====================== 11. 防守状态机 + 被锁定村民撤退 ======================
// 返回离防守核心最近的敌兵距离（块），无敌兵返回1e9
static double nearestEnemyBlockDist(const tagInfo& info)
{
    int cDR = (g_defTowerSN != -1) ? g_defTowerDR : g_tcDR;
    int cUR = (g_defTowerSN != -1) ? g_defTowerUR : g_tcUR;
    double best = 1e9;
    for(const auto& e : info.enemy_armies)
    {
        if(e.Blood <= 0) continue;
        double d = usrBlockDist((int)floor(e.DR/BLOCKSIDELENGTH+0.5),
                                (int)floor(e.UR/BLOCKSIDELENGTH+0.5), cDR, cUR);
        if(d < best) best = d;
    }
    return best;
}

// 返回当前锁定该村民的敌兵；没有敌兵锁定时返回-1。
static int enemyTargetingFarmer(const tagInfo& info, int farmerSN)
{
    for(const auto& e : info.enemy_armies)
        if(e.WorkObjectSN == farmerSN) return e.SN;
    return -1;
}

// 只统计基地附近仍可采集的浆果。第二波后浆果清空时，采果工逐个转种农田，
// 不再为了寻找远处浆果穿越地图。
static int berryFoodNearBase(const tagInfo& info)
{
    int total = 0;
    for(const auto& resource : info.resources)
    {
        if(resource.Type != RESOURCE_BUSH || resource.Cnt <= 0) continue;
        if(usrBlockDist(resource.BlockDR, resource.BlockUR, g_tcDR, g_tcUR) <=
           HUNT_SEARCH_RADIUS)
            total += resource.Cnt;
    }
    return total;
}

static bool isCompletedFarm(const tagInfo& info, int sn)
{
    return usableFarmTarget(info, sn);
}

// 六名核心食物工先采浆果；浆果耗尽后帮助处理已经击杀的瞪羚；
// 尸体处理完后再一人使用一片空闲农田。这里绝不选择活体瞪羚。
static int chooseBerryWorkerFoodTarget(const tagInfo& info, const tagFarmer& farmer)
{
    if(berryFoodNearBase(info) > 0)
    {
        int berry = usrBestGatherResource(info, RESOURCE_BUSH, farmer);
        if(berry >= 0) return berry;
    }
    int carcass = findHuntCarcass(info, farmer);
    if(carcass >= 0) return carcass;
    return findFreeFarm(info, farmer.SN);
}

// 判断一个SN是否仍属于我方存活的单位或建筑。
static bool isOurLiveObject(const tagInfo& info, int sn)
{
    if(sn < 0) return false;
    for(const auto& f : info.farmers)  if(f.SN == sn && f.Blood > 0) return true;
    for(const auto& a : info.armies)   if(a.SN == sn && a.Blood > 0) return true;
    for(const auto& b : info.buildings) if(b.SN == sn && b.Blood > 0) return true;
    return false;
}

// 防守不能只看敌人是否离开箭塔：敌人仍在追击我方单位，或靠近TC、箭塔、
// 任一村民时，都属于尚未解除的有效威胁。
// 返回威胁原因：1=锁定我方，2=基地22格内，3=村民10格内；0=无有效威胁。
// 第三波清场和安全计数共用此判定，避免守兵忽略阻止结束计数的附近敌人。
static int defenseThreatReason(const tagInfo& info, const tagArmy& e)
{
    if(e.Blood <= 0) return 0;
    double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
    double tx = (g_defTowerSN != -1 ? g_defTowerDR : g_tcDR)*BLOCKSIDELENGTH;
    double ty = (g_defTowerSN != -1 ? g_defTowerUR : g_tcUR)*BLOCKSIDELENGTH;
    const double coreRange2 = DEFENSE_SAFE_CORE_BLOCKS * DEFENSE_SAFE_CORE_BLOCKS
                              * BLOCKSIDELENGTH * BLOCKSIDELENGTH;
    const double farmerRange2 = DEFENSE_SAFE_FARMER_BLOCKS * DEFENSE_SAFE_FARMER_BLOCKS
                                * BLOCKSIDELENGTH * BLOCKSIDELENGTH;

    if(isOurLiveObject(info, e.WorkObjectSN)) return 1;
    if(d2(e.DR, e.UR, tcx, tcy) <= coreRange2 ||
       d2(e.DR, e.UR, tx, ty) <= coreRange2)
        return 2;

    for(const auto& f : info.farmers)
    {
        if(f.FarmerSort != FARMERTYPE_FARMER || f.Blood <= 0) continue;
        if(d2(e.DR, e.UR, f.DR, f.UR) <= farmerRange2) return 3;
    }
    return 0;
}

static bool hasActiveDefenseThreat(const tagInfo& info)
{
    for(const auto& enemy : info.enemy_armies)
        if(defenseThreatReason(info, enemy)) return true;
    return false;
}

// 锁定本轮敌军最初的接近方向。祭司使用固定方向站到塔后，避免因为最近敌人
// 在战斗中不断变化而围着箭塔来回走。
static void lockDefenseApproach(const tagInfo& info)
{
    int cDR = (g_defTowerSN != -1) ? g_defTowerDR : g_tcDR;
    int cUR = (g_defTowerSN != -1) ? g_defTowerUR : g_tcUR;
    const tagArmy* nearest = nullptr;
    double best = 1e30;
    double cx = cDR*BLOCKSIDELENGTH, cy = cUR*BLOCKSIDELENGTH;
    for(const auto& e : info.enemy_armies)
    {
        if(e.Blood <= 0) continue;
        double dd = d2(e.DR, e.UR, cx, cy);
        if(dd < best) { best = dd; nearest = &e; }
    }
    if(!nearest) return;

    g_defenseApproachDR = nearest->DR;
    g_defenseApproachUR = nearest->UR;
    g_defenseApproachLocked = true;
    g_priestLastMoveFrame = -1000; // 方向改变后允许祭司立即重新站位
    dbg(QString("[防守站位] 来敌方向取敌兵%1，祭司改到箭塔背面4格")
              .arg(nearest->SN));
}

static void updateDefense(const tagInfo& info)
{
    double nd = nearestEnemyBlockDist(info);
    bool activeThreat = hasActiveDefenseThreat(info);

    if(!g_defenseMode)
    {
        // 敌兵进入阵地或开始锁定我方对象时防守；4:40保底只触发尚未处理的第一波。
        bool enemyNear = g_aiframe >= FRAME_WATCH &&
                         (activeThreat || nd <= DEFENSE_TRIGGER_BLOCKS);
        bool firstWaveDeadline = !g_wave1Handled && g_aiframe >= FRAME_HARD_DEFENSE;
        if(enemyNear || firstWaveDeadline)
        {
            g_defenseMode = true;
            g_activeDefenseWave = !g_wave1Handled ? 1 : (!g_wave2Handled ? 2 : 3);
            g_calmFrames = 0;
            lockDefenseApproach(info);
            dbg(QString("[防守] 第%1波进入防守(最近敌兵%2格)，箭塔主防；只有被锁定村民撤离")
                      .arg(g_activeDefenseWave)
                      .arg(nd<1e8?QString::number(nd,'f',1):QString("?")));
        }
    }
    else
    {
        // 4:40保底可能先于敌军进入视野；看见第一名敌军后再锁定实际来敌方向。
        if(!g_defenseApproachLocked && !info.enemy_armies.empty())
            lockDefenseApproach(info);

        // 连续600帧同时满足：无人仍被追击、敌人远离基地且不靠近任何村民，才解除防守。
        if(!activeThreat) g_calmFrames++;
        else g_calmFrames = 0;
        // 第三波有残敌时每600帧说明一次阻塞原因，不放宽安全结束条件。
        static int lastThreatLog = -1000;
        if(g_wave2Handled && activeThreat && g_aiframe-lastThreatLog >= DEFENSE_SAFE_FRAMES)
        {
            for(const auto& enemy : info.enemy_armies)
                if(int reason = defenseThreatReason(info, enemy))
                {
                    dbg(QString("[防守阻塞] enemy=%1 reason=%2 target=%3 towerDistance=%4 tcDistance=%5")
                              .arg(enemy.SN).arg(reason).arg(enemy.WorkObjectSN)
                              .arg(sqrt(d2(enemy.DR,enemy.UR,g_defTowerDR*BLOCKSIDELENGTH,g_defTowerUR*BLOCKSIDELENGTH))/BLOCKSIDELENGTH,0,'f',1)
                              .arg(sqrt(d2(enemy.DR,enemy.UR,g_tcDR*BLOCKSIDELENGTH,g_tcUR*BLOCKSIDELENGTH))/BLOCKSIDELENGTH,0,'f',1));
                    break;
                }
            lastThreatLog = g_aiframe;
        }
        if(g_calmFrames == 100 || g_calmFrames == 300)
            dbg(QString("[防守安全计数] count=%1/%2").arg(g_calmFrames).arg(DEFENSE_SAFE_FRAMES));
        if(g_calmFrames >= DEFENSE_SAFE_FRAMES)
        {
            g_defenseMode = false;
            int clearedWave = g_activeDefenseWave;
            if(clearedWave <= 0)
                clearedWave = !g_wave1Handled ? 1 : (!g_wave2Handled ? 2 : 3);
            if(clearedWave == 1) g_wave1Handled = true;
            if(clearedWave == 2) g_wave2Handled = true;
            g_activeDefenseWave = 0;
            g_calmFrames = 0;
            g_defenseApproachLocked = false;
            if(clearedWave == 1)
                dbg(QString("[防守] 第一波威胁解除，铜器时代村民目标为%1")
                          .arg(PRE_WAVE2_LAND_FARMERS));
            else if(clearedWave == 2)
                dbg(QString("[防守] 第二波威胁解除，村民目标提升至%1")
                          .arg(TARGET_LAND_FARMERS));
            else
                dbg(QString("[防守] 当前波次威胁解除"));
        }
    }

    // 防守期：普通村民保持原工作；只有被敌兵WorkObjectSN锁定的村民撤向TC。
    if(g_defenseMode)
    {
        static unordered_map<int,int> lastRetreat;
        for(const auto& f : info.farmers)
        {
            if(f.FarmerSort != FARMERTYPE_FARMER) continue;
            int attacker = enemyTargetingFarmer(info, f.SN);
            if(attacker == -1) continue;

            double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
            double dd = d2(f.DR, f.UR, tcx, tcy);
            if(dd > 2.0*2.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
            {
                auto it = lastRetreat.find(f.SN);
                int last = (it==lastRetreat.end()) ? -99999 : it->second;
                if(g_aiframe - last >= 250 && snFree(f.SN))
                {
                    orderMove(f.SN, tcx, tcy);
                    lastRetreat[f.SN] = g_aiframe;
                    dbg(QString("[防守撤离] 村民%1被敌兵%2锁定，撤向市镇中心")
                              .arg(f.SN).arg(attacker));
                }
            }
        }
    }
}

static bool isDangerousRangedEnemy(int sort)
{
    return sort == AT_BOWMAN || sort == AT_IMPROVED ||
           sort == AT_COMPOSITE_BOWMAN || sort == AT_CHARIOT_ARCHER;
}

// 第二波让箭塔承担主要伤害。人口房没有待处理任务时，固定建筑工持续维修箭塔；
// 建筑工自己被锁定时仍由村民撤退逻辑接管。
static void manageCombatTowerRepair(const tagInfo& info)
{
    if(!g_defenseMode || g_defTowerSN == -1 || g_builderSN == -1) return;
    // 后期石材耗尽时修理无法执行，不能用无效修塔指令反复打断已恢复的施工。
    if(g_wave2Handled && info.Stone <= 0) return;
    const tagBuilding* tower = getBuilding(info, g_defTowerSN);
    const tagFarmer* builder = usrGetFarmer(info, g_builderSN);
    if(!tower || !builder || tower->Percent < 100 || tower->Blood <= 0) return;

    if(tower->Blood >= tower->MaxBlood)
    {
        if(g_towerRepairStarted)
            dbg(QString("[箭塔战斗维修完成] tower=%1 blood=%2/%3")
                      .arg(tower->SN).arg(tower->Blood).arg(tower->MaxBlood));
        g_towerRepairStarted = false;
        return;
    }

    // 紧急人口房和已经开工的房屋仍优先，不能把房屋建到一半永久丢下。
    if(hasPendingBuildType(BUILDING_HOME) || unfinishedBuilding(info, BUILDING_HOME)) return;
    if(enemyTargetingFarmer(info, g_builderSN) != -1 || !snFree(g_builderSN)) return;
    if(g_aiframe-g_lastCombatTowerRepairFrame < 100) return;

    orderBuilderWork(g_builderSN, tower->SN);
    g_lastCombatTowerRepairFrame = g_aiframe;
    if(!g_towerRepairStarted)
        dbg(QString("[箭塔战斗维修] builder=%1 tower=%2 blood=%3/%4")
                  .arg(g_builderSN).arg(tower->SN)
                  .arg(tower->Blood).arg(tower->MaxBlood));
    g_towerRepairStarted = true;
}

// 玩家箭塔不会自动选择攻击对象，必须显式下达HumanAction。
// 祭司被多人锁定时，箭塔先逐个攻击追兵；确认该追兵已经转向箭塔后，
// 立即切换下一名。全部追兵离开祭司后，再固定高级远程目标供祭司转换。
static void manageDefenseTower(const tagInfo& info)
{
    if(!g_defenseMode || g_defTowerSN == -1)
    {
        g_towerRescueSweepActive = false;
        g_towerNoPriestThreatFrames = 0;
        return;
    }
    const tagBuilding* tower = getBuilding(info, g_defTowerSN);
    if(!tower || tower->Percent < 100 || tower->Blood <= 0) return;

    const double range2 = TOWER_ATTACK_RANGE_BLOCKS * TOWER_ATTACK_RANGE_BLOCKS
                          * BLOCKSIDELENGTH * BLOCKSIDELENGTH;
    for(const auto& e : info.enemy_armies)
        if(e.WorkObjectSN == g_defTowerSN && g_towerAggroLogged.insert(e.SN).second)
            dbg(QString("[箭塔拉敌成功] enemy=%1 targetTower=%2")
                      .arg(e.SN).arg(g_defTowerSN));

    bool anyPriestAttacker = false;
    bool unsecuredRangedNearby = false;
    const tagArmy* rescueTarget = nullptr;
    int rescuePriority = 99;
    double rescueDistance = 1e30;
    const double watch2 = 14.0 * 14.0 * BLOCKSIDELENGTH * BLOCKSIDELENGTH;
    for(const auto& enemy : info.enemy_armies)
    {
        double distance = d2(enemy.DR, enemy.UR,
                             tower->BlockDR*BLOCKSIDELENGTH,
                             tower->BlockUR*BLOCKSIDELENGTH);
        if(isDangerousRangedEnemy(enemy.Sort) &&
           enemy.WorkObjectSN != g_defTowerSN && distance <= watch2)
            unsecuredRangedNearby = true;

        if(enemy.WorkObjectSN != g_priestSN) continue;
        anyPriestAttacker = true;

        int priority = enemy.SN == g_priestDangerAttacker ? 0
                     : isDangerousRangedEnemy(enemy.Sort) ? 1 : 2;
        if(priority < rescuePriority ||
           (priority == rescuePriority && distance < rescueDistance))
        {
            rescueTarget = &enemy;
            rescuePriority = priority;
            rescueDistance = distance;
        }
    }

    if(anyPriestAttacker || unsecuredRangedNearby)
    {
        g_towerRescueSweepActive = true;
        g_towerNoPriestThreatFrames = 0;
    }
    else if(g_towerRescueSweepActive)
        ++g_towerNoPriestThreatFrames;

    // 即使追兵暂时还在7格射程外，也先让箭塔锁定它。建筑不会移动，
    // 但目标进入实际射程后会立刻开火，不能在等待期间转打其他敌人。
    // 确认追兵的WorkObjectSN改成箭塔后，下一帧再切换下一名。
    if(rescueTarget)
    {
        if(tower->Project != rescueTarget->SN && snFree(g_defTowerSN))
        {
            orderAction(info, g_defTowerSN, rescueTarget->SN);
            g_towerLastAttackTarget = rescueTarget->SN;
            g_towerLastAttackFrame = g_aiframe;
            dbg(QString("[箭塔救援祭司] tower=%1 attacker=%2 sort=%3 distance=%4 previousTarget=%5")
                      .arg(g_defTowerSN).arg(rescueTarget->SN).arg(rescueTarget->Sort)
                      .arg(sqrt(rescueDistance)/BLOCKSIDELENGTH,0,'f',1)
                      .arg(tower->Project));
        }
        return;
    }

    // 危险远程兵即使暂时转攻我方转化兵，也仍可能很快重新锁定祭司。
    // 只要进入箭塔射程且尚未攻击箭塔，就立即拉到箭塔上。
    const tagArmy* unpulledRanged = nullptr;
    double unpulledDistance = 1e30;
    for(const auto& enemy : info.enemy_armies)
    {
        if(!isDangerousRangedEnemy(enemy.Sort) ||
           enemy.WorkObjectSN == g_defTowerSN) continue;
        double distance = d2(enemy.DR, enemy.UR,
                             tower->BlockDR*BLOCKSIDELENGTH,
                             tower->BlockUR*BLOCKSIDELENGTH);
        if(distance <= range2 && distance < unpulledDistance)
        {
            unpulledRanged = &enemy;
            unpulledDistance = distance;
        }
    }
    if(unpulledRanged)
    {
        g_towerRescueSweepActive = true;
        g_towerNoPriestThreatFrames = 0;
        if(tower->Project != unpulledRanged->SN && snFree(g_defTowerSN))
        {
            orderAction(info, g_defTowerSN, unpulledRanged->SN);
            g_towerLastAttackTarget = unpulledRanged->SN;
            g_towerLastAttackFrame = g_aiframe;
            dbg(QString("[箭塔预拉远程] tower=%1 enemy=%2 previousTarget=%3")
                      .arg(g_defTowerSN).arg(unpulledRanged->SN).arg(tower->Project));
        }
        return;
    }

    // 当前祭司追兵仍在7格射程外时，祭司继续把它向塔下引；箭塔暂不切换
    // 到普通目标。若另有危险远程兵先进入射程，上面的预拉逻辑会先处理它。
    if(anyPriestAttacker) return;

    if(g_towerRescueSweepActive)
    {
        if(g_towerNoPriestThreatFrames < TOWER_RESCUE_STABLE_FRAMES) return;
        g_towerRescueSweepActive = false;
        g_towerNoPriestThreatFrames = 0;
        dbg(QString("[箭塔救援完成] 连续%1帧无敌军锁定祭司，开始固定转换目标")
                  .arg(TOWER_RESCUE_STABLE_FRAMES));
    }

    const tagArmy* current = nullptr;
    for(const auto& e : info.enemy_armies)
        if(e.SN == tower->Project &&
           d2(e.DR, e.UR,
              tower->BlockDR*BLOCKSIDELENGTH,
              tower->BlockUR*BLOCKSIDELENGTH) <= range2)
        { current = &e; break; }

    const tagArmy* target = nullptr;
    int bestPriority = 99;
    double bestDistance = 1e30;
    for(const auto& e : info.enemy_armies)
    {
        double dd = d2(e.DR, e.UR,
                       tower->BlockDR*BLOCKSIDELENGTH,
                       tower->BlockUR*BLOCKSIDELENGTH);
        if(dd > range2) continue;

        // 追兵清空后先固定高级远程目标，其次救村民，再处理攻击箭塔的敌人。
        int priority = 3;
        if(isDangerousRangedEnemy(e.Sort)) priority = 0;
        else if(usrGetFarmer(info, e.WorkObjectSN)) priority = 1;
        else if(e.WorkObjectSN == g_defTowerSN) priority = 2;
        if(priority < bestPriority || (priority == bestPriority && dd < bestDistance))
        {
            target = &e;
            bestPriority = priority;
            bestDistance = dd;
        }
    }

    // 当前目标与最佳目标同级或更重要时保持锁定，给箭塔和祭司稳定的处理时间。
    if(current)
    {
        int currentPriority = isDangerousRangedEnemy(current->Sort) ? 0
                            : usrGetFarmer(info, current->WorkObjectSN) ? 1
                            : current->WorkObjectSN == g_defTowerSN ? 2 : 3;
        if(!target || currentPriority <= bestPriority) return;
    }

    if(!target || target->SN == tower->Project || !snFree(g_defTowerSN)) return;

    // 指令尚未反映到Project时短暂等待，防止重复提交打断箭塔攻击关系。
    if(target->SN == g_towerLastAttackTarget &&
       g_aiframe-g_towerLastAttackFrame < 50) return;

    orderAction(info, g_defTowerSN, target->SN);
    g_towerLastAttackTarget = target->SN;
    g_towerLastAttackFrame = g_aiframe;
    dbg(QString("[箭塔攻击] tower=%1 target=%2 enemyPreviousTarget=%3")
              .arg(g_defTowerSN).arg(target->SN).arg(target->WorkObjectSN));
}

static int rangedScoutPriority(int sort)
{
    if(sort == AT_BOWMAN || sort == AT_IMPROVED) return 0;
    if(sort == AT_COMPOSITE_BOWMAN) return 1;
    if(sort == AT_CHARIOT_ARCHER) return 2;
    return 99;
}

// 第二波后由一名远程兵代替祭司探路。发现敌军、被锁定、低血或接近第三波时
// 立即返回箭塔；返回后不再外出，直接加入后续防守。
static void manageRangedScout(const tagInfo& info)
{
    if(!g_wave2Handled || g_rangedScoutState == RANGED_SCOUT_LOST) return;

    if(g_rangedScoutSN == -1)
    {
        if(g_defenseMode) return;
        const tagArmy* candidate = nullptr;
        int bestPriority = 99;
        for(const auto& army : info.armies)
        {
            int priority = rangedScoutPriority(army.Sort);
            if(priority < bestPriority ||
               (priority == bestPriority && candidate && army.Blood > candidate->Blood))
            {
                candidate = &army;
                bestPriority = priority;
            }
        }
        if(!candidate || bestPriority == 99) return;

        g_rangedScoutSN = candidate->SN;
        g_rangedScoutState = RANGED_SCOUT_EXPLORE;
        g_rangedScoutLastMoveFrame = -1000;
        dbg(QString("[远程探路开始] unit=%1 sort=%2 radius=%3")
                  .arg(candidate->SN).arg(candidate->Sort)
                  .arg(RANGED_SCOUT_RADIUS_BLOCKS,0,'f',0));
    }

    const tagArmy* scout = usrGetArmy(info, g_rangedScoutSN);
    if(!scout)
    {
        dbg(QString("[远程探路损失] unit=%1 已阵亡，不再派出第二名探路兵")
                  .arg(g_rangedScoutSN));
        g_rangedScoutSN = -1;
        g_rangedScoutState = RANGED_SCOUT_LOST;
        return;
    }

    if(g_rangedScoutState == RANGED_SCOUT_EXPLORE)
    {
        int nearbyEnemy = -1;
        double nearest = RANGED_SCOUT_ENEMY_RANGE_BLOCKS *
                         RANGED_SCOUT_ENEMY_RANGE_BLOCKS *
                         BLOCKSIDELENGTH * BLOCKSIDELENGTH;
        bool targeted = false;
        for(const auto& enemy : info.enemy_armies)
        {
            if(enemy.WorkObjectSN == g_rangedScoutSN) targeted = true;
            double distance = d2(scout->DR, scout->UR, enemy.DR, enemy.UR);
            if(distance < nearest)
            {
                nearest = distance;
                nearbyEnemy = enemy.SN;
            }
        }

        QString reason;
        if(g_defenseMode) reason = QString("defense");
        else if(targeted) reason = QString("targeted");
        else if(scout->Blood * 100 <= scout->MaxBlood *
                RANGED_SCOUT_RETURN_HEALTH_PERCENT) reason = QString("low_health");
        else if(nearbyEnemy != -1) reason = QString("enemy_%1").arg(nearbyEnemy);
        else if(g_aiframe >= FRAME_RANGED_SCOUT_RECALL) reason = QString("wave3_deadline");

        if(!reason.isEmpty())
        {
            g_rangedScoutState = RANGED_SCOUT_RETURN;
            g_rangedScoutLastMoveFrame = -1000;
            dbg(QString("[远程探路返程] unit=%1 reason=%2 blood=%3/%4")
                      .arg(g_rangedScoutSN).arg(reason)
                      .arg(scout->Blood).arg(scout->MaxBlood));
        }
    }

    if(g_rangedScoutState == RANGED_SCOUT_RETURN)
    {
        double distance = d2(scout->DR, scout->UR, g_priestPointDR, g_priestPointUR);
        if(distance <= 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
        {
            g_rangedScoutState = RANGED_SCOUT_HOLD;
            dbg(QString("[远程探路归队] unit=%1 已返回箭塔").arg(g_rangedScoutSN));
            return;
        }
        if(g_aiframe-g_rangedScoutLastMoveFrame >= 100 && snFree(g_rangedScoutSN))
        {
            orderMove(g_rangedScoutSN, g_priestPointDR, g_priestPointUR);
            g_rangedScoutLastMoveFrame = g_aiframe;
        }
        return;
    }

    if(g_rangedScoutState != RANGED_SCOUT_EXPLORE) return;

    static const int dx8[8] = {0, 1, 1, 1, 0,-1,-1,-1};
    static const int dy8[8] = {1, 1, 0,-1,-1,-1, 0, 1};
    int k = g_rangedScoutPoint % 8;
    double tx = static_cast<double>(g_tcDR*BLOCKSIDELENGTH) +
                dx8[k]*RANGED_SCOUT_RADIUS_BLOCKS*BLOCKSIDELENGTH;
    double ty = static_cast<double>(g_tcUR*BLOCKSIDELENGTH) +
                dy8[k]*RANGED_SCOUT_RADIUS_BLOCKS*BLOCKSIDELENGTH;
    double maxDR = ((int)info.theMap->size()-1)*BLOCKSIDELENGTH;
    double maxUR = ((int)(*info.theMap)[0].size()-1)*BLOCKSIDELENGTH;
    tx = max(0.5*BLOCKSIDELENGTH, min(maxDR, tx));
    ty = max(0.5*BLOCKSIDELENGTH, min(maxUR, ty));

    if(d2(scout->DR, scout->UR, tx, ty) <=
       4.0*4.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH ||
       g_aiframe-g_rangedScoutLastMoveFrame >= 400)
    {
        if(snFree(g_rangedScoutSN))
        {
            orderMove(g_rangedScoutSN, tx, ty);
            g_rangedScoutLastMoveFrame = g_aiframe;
            g_rangedScoutPoint++;
        }
    }
}

// 主战斗结束后才扩大到22格清场，不能在祭司仍被追击时把护卫提前派走。
static bool canClearDefenseStragglers(const tagInfo& info)
{
    static bool closeBattleSeen = false;
    if(!g_wave2Handled || !g_defenseMode || g_activeDefenseWave < 3)
    {
        closeBattleSeen = false;
        return false;
    }
    const double radius = SIEGE_INTERCEPT_RADIUS_BLOCKS*BLOCKSIDELENGTH;
    bool safe = true;
    for(const auto& enemy : info.enemy_armies)
    {
        if(enemy.Blood <= 0) continue;
        if(d2(enemy.DR,enemy.UR,g_defTowerDR*BLOCKSIDELENGTH,g_defTowerUR*BLOCKSIDELENGTH) <= radius*radius)
        {
            if(!closeBattleSeen) dbg(QString("[第三波接敌] enemy=%1，主战斗开始").arg(enemy.SN));
            closeBattleSeen = true;
            safe = false;
        }
        if(g_priestSN >= 0 && enemy.WorkObjectSN == g_priestSN) safe = false;
    }
    // 敌军尚在接近、塔周围暂时空着，不代表主战斗已经结束。
    return closeBattleSeen && safe;
}

// ====================== 12. 我方士兵：防守期集火，平时集结塔下 ======================
static void manageSoldiers(const tagInfo& info)
{
    int cDR = (g_defTowerSN != -1) ? g_defTowerDR : g_tcDR;
    int cUR = (g_defTowerSN != -1) ? g_defTowerUR : g_tcUR;
    double cx = cDR*BLOCKSIDELENGTH, cy = cUR*BLOCKSIDELENGTH;
    static unordered_map<int,int> lastRally;
    static unordered_map<int,int> lastDefenseOrder;
    static bool siegeTowerLost = false; // 失守时只补记一次当前拦截状态。
    bool clearStragglers = canClearDefenseStragglers(info);

    const tagArmy* siege = findTowerSiege(info, g_defTowerDR*BLOCKSIDELENGTH,
                                        g_defTowerUR*BLOCKSIDELENGTH,
                                        SIEGE_INTERCEPT_RADIUS_BLOCKS,
                                        g_siegeInterceptTarget);
    const set<int> previousInterceptors = g_siegeInterceptors;
    int previousTarget = g_siegeInterceptTarget;
    g_siegeInterceptors.clear();
    g_siegeInterceptTarget = siege ? siege->SN : -1;
    if(siege)
    {
        vector<const tagArmy*> candidates;
        int defenders = 0;
        const double radius = SIEGE_INTERCEPT_RADIUS_BLOCKS*BLOCKSIDELENGTH;
        auto meleePriority = [](int sort) {
            if(sort == AT_CAVALRY || sort == AT_CHARIOT) return 0;
            if(sort == AT_SCOUT) return 1;
            if(sort == AT_CLUBMAN || sort == AT_SWORDSMAN ||
               sort == AT_BROADSWORDSMAN || sort == AT_HOPLITE) return 2;
            return -1;
        };
        for(const auto& army : info.armies)
        {
            if(army.Sort == AT_PRIEST || army.Sort == AT_SHIP || army.Blood <= 0 ||
               army.Blood*100 < army.MaxBlood*SIEGE_INTERCEPT_MIN_HEALTH_PERCENT ||
               d2(army.DR, army.UR, g_defTowerDR*BLOCKSIDELENGTH,
                  g_defTowerUR*BLOCKSIDELENGTH) > radius*radius ||
               (army.SN == g_rangedScoutSN &&
                (g_rangedScoutState == RANGED_SCOUT_EXPLORE ||
                 g_rangedScoutState == RANGED_SCOUT_RETURN))) continue;
            ++defenders;
            if(meleePriority(army.Sort) >= 0) candidates.push_back(&army);
        }
        // 保持现有拦截成员，避免每帧因距离变化换人；新增成员优先选快速近战兵。
        sort(candidates.begin(), candidates.end(), [&](const tagArmy* a, const tagArmy* b) {
            if(previousInterceptors.count(a->SN) != previousInterceptors.count(b->SN))
                return previousInterceptors.count(a->SN) > previousInterceptors.count(b->SN);
            if(meleePriority(a->Sort) != meleePriority(b->Sort))
                return meleePriority(a->Sort) < meleePriority(b->Sort);
            double da = d2(a->DR, a->UR, siege->DR, siege->UR);
            double db = d2(b->DR, b->UR, siege->DR, siege->UR);
            return da == db ? a->SN < b->SN : da < db;
        });
        int count = min(SIEGE_INTERCEPT_MAX_UNITS, max(0, defenders-1));
        for(const auto* army : candidates)
            if((int)g_siegeInterceptors.size() < count) g_siegeInterceptors.insert(army->SN);
    }
    const tagBuilding* tower = getBuilding(info, g_defTowerSN);
    bool towerLost = siege && (!tower || tower->Blood <= 0);
    if(previousTarget != g_siegeInterceptTarget || previousInterceptors != g_siegeInterceptors ||
       towerLost != siegeTowerLost)
    {
        QString members;
        for(int sn : g_siegeInterceptors) members += QString(" %1").arg(sn);
        const tagArmy* oldEnemy = nullptr;
        for(const auto& enemy : info.enemy_armies)
            if(enemy.SN == previousTarget) { oldEnemy = &enemy; break; }
        dbg(QString("[第三波投石车拦截] target=%1 units=%2 previous=%3 towerHP=%4 previousEnemyTarget=%5")
                  .arg(g_siegeInterceptTarget).arg(members.isEmpty() ? QString("none") : members)
                  .arg(previousTarget).arg(tower ? tower->Blood : 0)
                  .arg(oldEnemy ? oldEnemy->WorkObjectSN : -1));
    }
    siegeTowerLost = towerLost;
    set<int> cleanupUnits;
    if(clearStragglers)
    {
        vector<int> guards;
        for(const auto& army : info.armies)
            if(army.Blood > 0 && army.Sort != AT_PRIEST && army.Sort != AT_SHIP &&
               !g_siegeInterceptors.count(army.SN) &&
               !(army.SN == g_rangedScoutSN &&
                 (g_rangedScoutState == RANGED_SCOUT_EXPLORE || g_rangedScoutState == RANGED_SCOUT_RETURN)))
                guards.push_back(army.SN);
        sort(guards.begin(), guards.end()); // 固定SN顺序，避免每帧换人；至少留一名护卫。
        int count = min(DEFENSE_CLEANUP_MAX_UNITS, max(0, (int)guards.size()-1));
        for(int i=0; i<count; ++i) cleanupUnits.insert(guards[i]);
    }
    for(int sn : previousInterceptors)
    {
        const tagArmy* army = usrGetArmy(info, sn);
        if(army && army->Blood > 0 && !g_siegeInterceptors.count(sn) &&
           snFree(sn))
        {
            orderMove(sn, g_priestPointDR, g_priestPointUR);
            lastDefenseOrder[sn] = g_aiframe;
            dbg(QString("[投石车拦截撤回] unit=%1，停止追击并回到护卫位置").arg(sn));
        }
    }

    for(const auto& a : info.armies)
    {
        if(a.Sort == AT_PRIEST) continue;   // 祭司单独管
        if(a.SN == g_rangedScoutSN &&
           (g_rangedScoutState == RANGED_SCOUT_EXPLORE ||
            g_rangedScoutState == RANGED_SCOUT_RETURN))
            continue;

        if(g_defenseMode)
        {
            if(siege && g_siegeInterceptors.count(a.SN))
            {
                int last = lastDefenseOrder.count(a.SN) ? lastDefenseOrder[a.SN] : -99999;
                if(a.WorkObjectSN != siege->SN && g_aiframe-last >= 50 && snFree(a.SN))
                {
                    // 拦截兵允许打断普通攻击；其他士兵继续走原有的祭司护卫逻辑。
                    orderAction(info, a.SN, siege->SN);
                    lastDefenseOrder[a.SN] = g_aiframe;
                    dbg(QString("[投石车拦截下单] unit=%1 target=%2").arg(a.SN).arg(siege->SN));
                }
                continue;
            }
            // 转化兵先救祭司，再处理复合弓、战车弓等远程兵；最后攻击其他近敌。
            int target = -1;
            double best = 1e30;
            int bestPriority = 99;
            double r14 = 14.0*14.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH;
            for(const auto& e : info.enemy_armies)
            {
                if(e.Blood <= 0) continue;
                double dc = d2(cx, cy, e.DR, e.UR);
                if(cleanupUnits.count(a.SN))
                {
                    // 覆盖TC和塔的同一个22格威胁区；不追到基地范围以外。
                    dc = min(dc, d2(g_tcDR*BLOCKSIDELENGTH,g_tcUR*BLOCKSIDELENGTH,e.DR,e.UR));
                    double radius = DEFENSE_SAFE_CORE_BLOCKS*BLOCKSIDELENGTH;
                    if(dc > radius*radius || !defenseThreatReason(info, e)) continue;
                }
                else if(dc > r14) continue;

                int priority = 3;
                if(e.WorkObjectSN == g_priestSN) priority = 0;
                else if(isDangerousRangedEnemy(e.Sort)) priority = 1;
                else if(usrGetFarmer(info, e.WorkObjectSN)) priority = 2;
                if(priority < bestPriority ||
                   (priority == bestPriority && dc < best))
                {
                    bestPriority = priority;
                    best = dc;
                    target = e.SN;
                }
            }

            // 正在攻击时通常不打断；如果出现新的祭司追兵，则立即转火救援。
            bool urgentPriestRescue = bestPriority == 0;
            int last = lastDefenseOrder.count(a.SN) ? lastDefenseOrder[a.SN] : -99999;
            if(target != -1 && a.WorkObjectSN != target &&
               (a.NowState != HUMAN_STATE_ATTACKING || urgentPriestRescue) &&
               g_aiframe-last >= 50 && snFree(a.SN))
            {
                orderAction(info, a.SN, target);
                lastDefenseOrder[a.SN] = g_aiframe;
                if(urgentPriestRescue)
                    dbg(QString("[转化兵护卫] unit=%1 target=%2 保护祭司")
                              .arg(a.SN).arg(target));
                else if(cleanupUnits.count(a.SN))
                    dbg(QString("[第三波清场] unit=%1 target=%2 distance=%3")
                              .arg(a.SN).arg(target).arg(sqrt(best)/BLOCKSIDELENGTH,0,'f',1));
            }
        }
        else
        {
            // 平时集结到塔后安全点（500帧节流）
            double dd = d2(a.DR, a.UR, g_priestPointDR, g_priestPointUR);
            if(dd > 4.0*4.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
            {
                int last = lastRally.count(a.SN) ? lastRally[a.SN] : -99999;
                if(g_aiframe - last >= 500)
                {
                    orderMove(a.SN, g_priestPointDR, g_priestPointUR);
                    lastRally[a.SN] = g_aiframe;
                }
            }
        }
    }
}

// ====================== 13. 空闲村民按岗位续任务（防挂机） ======================
static void idleResume(const tagInfo& info)
{
    for(auto& kv : g_role)
    {
        int sn = kv.first, role = kv.second;
        const tagFarmer* f = usrGetFarmer(info, sn);
        if(!f) { kv.second = ROLE_NONE; continue; }     // 人没了，清岗位
        // 被敌人锁定的村民由防守逻辑撤离；其他村民即使在防守期也继续工作。
        if(g_defenseMode && enemyTargetingFarmer(info, sn) != -1) continue;
        if(f->NowState != HUMAN_STATE_IDLE || farmerHasPendingGatherOrder(sn)) continue;

        int t = -1;
        if(role == ROLE_WOOD)
            t = usrBestGatherResource(info, RESOURCE_TREE, *f);
        else if(role == ROLE_BUSH)
        {
            if(g_berryFarmWorkers.count(sn))
            {
                t = chooseBerryWorkerFoodTarget(info, *f);
                if(isCompletedFarm(info, t))
                {
                    kv.second = ROLE_FARM;
                    dbg(QString("[浆果转农田] farmer=%1 farm=%2").arg(sn).arg(t));
                }
                if(t < 0)
                    t = usrBestGatherResource(info, RESOURCE_TREE, *f);
            }
            else
            {
                t = chooseFoodTarget(info, *f);
            }
        }
        else if(role == ROLE_FARM)
        {
            // 核心食物工耗尽一块田后仍留在食物岗位，选择新田或尚存的天然食物。
            t = g_berryFarmWorkers.count(sn) ? chooseBerryWorkerFoodTarget(info, *f)
                                           : findFreeFarm(info, sn);
            if(t == -1 && !g_berryFarmWorkers.count(sn))
                t = usrBestGatherResource(info, RESOURCE_BUSH, *f);
        }
        else if(role == ROLE_GOLD)
            t = usrBestGatherResource(info, RESOURCE_GOLD, *f);
        else if(role == ROLE_STONE)
            t = usrBestGatherResource(info, RESOURCE_STONE, *f);
        // ROLE_BUILDER 由 builderQueue 接管；ROLE_HUNTER 由 manageHunters 接管。
        if(t != -1) orderAction(info, sn, t);
    }
}

// ====================== 主入口（每帧一次） ======================
void UsrAI::processData()
{
    tagInfo info = getInfo();
    if(info.theMap == nullptr) return;

    g_ai = this;                 // 供文件级辅助函数转发命令/日志
    g_aiframe = info.GameFrame;
    g_orderedThisFrame.clear();
    processBuildResults(info);
    processFarmResumeResult(info);
    processGatherResults(info);
    cleanupGatherTargets(info);
    cleanupFarmerRoles(info);
    processFarmerProduction(info);
    logEconomicProgress(info);

    // 低频状态日志
    if(g_aiframe % 300 == 0)
    {
        dbg(QString("[%1帧 %2:%3] 人口%4/%5 食%6 木%7 石%8 金%9 时代%10 防%11")
                  .arg(g_aiframe).arg(g_aiframe/FPS/60,2,10,QChar('0')).arg(g_aiframe/FPS%60,2,10,QChar('0'))
                  .arg(info.Human_Num,0,'f',1).arg(info.Human_MaxNum)
                  .arg(info.Meat).arg(info.Wood).arg(info.Stone).arg(info.Gold)
                  .arg(info.civilizationStage).arg(g_defenseMode?1:0));
    }

    // 0) 阵地初始化（只做一次）
    if(g_tcDR < 0) setupBase(info);
    if(g_tcDR < 0) return;
    refreshDefTower(info);

    // 1) 科技 / 造村民 / 升时代 / 造兵（建筑指令）
    techAndProduction(info);

    // 2) 开局分工
    initialAssign(info);

    // 3) 前两波暂停建设；之后安全的建筑工可继续扩容和补田，避免残敌锁住经济。
    bool pauseBuilds = pauseEconomicBuilds(info);
    static bool lastBuildPause = false;
    if(g_wave2Handled && pauseBuilds != lastBuildPause)
        dbg(QString("[经济建设%1] defense=%2 builder=%3")
                  .arg(pauseBuilds ? QString("暂停") : QString("恢复"))
                  .arg(g_defenseMode ? 1 : 0).arg(g_builderSN));
    lastBuildPause = pauseBuilds;
    builderQueue(info, pauseBuilds);

    // 4) 新村民分配（每帧最多1个）；防守期也正常分工，避免在TC旁闲置
    if(g_initDone) assignNewFarmer(info);

    // 升铜前临时提高食物工比例，铜器完成后恢复原有经济结构。
    if(g_initDone && !g_defenseMode) manageUpgradeFoodWorkers(info);

    // 六名核心食物工依次采浆果、处理尸体，最后一人一片农田。
    if(g_initDone && (!g_defenseMode || g_wave2Handled)) manageBerryFarmWorkers(info);

    // 铜器时代且金矿仓库可用后，将两个普通伐木工转为采金。
    if(g_initDone && (!g_defenseMode || g_wave2Handled)) manageGoldMiners(info);

    // 5) 双人猎：防守期照常工作，只有被敌军锁定时才由函数内部暂停并撤离
    if(g_initDone) manageHunters(info);

    // 6) 防守状态机（只撤退被敌人锁定的村民）
    updateDefense(info);

    // 防守中由固定建筑工维修箭塔；紧急人口房仍由建造链优先处理。
    manageCombatTowerRepair(info);

    // 箭塔先逐个拉走祭司追兵，再固定高威胁目标供祭司转换。
    manageDefenseTower(info);

    // 7) 祭司（探路/回防/转换）
    managePriest(info);

    // 第二波后由一名远程兵探路；祭司留守箭塔。
    manageRangedScout(info);

    // 8) 士兵集结/集火（正在探路或返程的单位由探路状态机单独管理）
    manageSoldiers(info);

    // 9) 空闲续任务；耗尽农田由建造链补建，核心食物工接手新田。
    idleResume(info);
}

