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
bool g_needNewTower = false;
bool g_towerTechIssued = false;
bool g_towerTechBusy = false;
bool g_towerTechDone = false;

bool g_defenseMode = false;
int  g_calmFrames = 0;
bool g_wave1Handled = false;

int  g_priestSN = -1;
int  g_priestScoutIdx = 0;
int  g_priestLastMoveFrame = -1000;
int  g_priestConvertTarget = -1;
double g_priestPointDR = 0, g_priestPointUR = 0;
double g_rallyDR = 0, g_rallyUR = 0;

unordered_map<int,int> g_lastBuildTry;
set<int> g_tmpBuilders;
unordered_map<int,int> g_tmpRetry;
bool g_huntStockBuilt = false;
bool g_goldStockBuilt = false;

bool g_woodTechIssued = false;
bool g_compositeIssued = false;

set<int> g_orderedThisFrame;

struct PendingBuild { int builder, type, dr, ur, frame; };
static unordered_map<int, PendingBuild> g_pendingBuild; // order id -> build
static unordered_map<int, PendingBuild> g_acceptedBuild; // builder SN -> build
static set<int> g_failedBuildSites;
static unordered_map<int,int> g_farmerGatherTargets;
static unordered_map<int,int> g_gatherOrders;
static unordered_map<int,int> g_gatherOrderFrames;
static int g_farmerOrderId = -1;
static int g_farmerOrderBaseCount = -1;
static int g_farmerOrderFrame = -1;
static bool g_farmerCapLogged = false;
static int g_lastCivStage = -1;
static set<int> g_loggedCompletedBuildings;
static set<int> g_upgradeFoodWorkers;

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

            bool busy = false;
            for(const auto& b : info.buildings)           // 避开所有建筑(含在建)
                if(abs(b.BlockDR-dr) <= 1 && abs(b.BlockUR-ur) <= 1) { busy = true; break; }
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

static void cleanupGatherTargets(const tagInfo& info)
{
    for(auto it=g_farmerGatherTargets.begin(); it!=g_farmerGatherTargets.end(); )
    {
        const tagFarmer* farmer = usrGetFarmer(info, it->first);
        if(!farmer || !usableResource(info, it->second) || it->first == g_builderSN ||
           (g_role.count(it->first) && g_role[it->first] == ROLE_TMPB))
            it = g_farmerGatherTargets.erase(it);
        else ++it;
    }
}

static void cleanupFarmerRoles(const tagInfo& info)
{
    for(auto& entry : g_role)
        if(!usrGetFarmer(info, entry.first)) entry.second = ROLE_NONE;
}

int usrBestGatherResource(const tagInfo& info, int resType, const tagFarmer& farmer)
{
    unordered_map<int,int> load;
    for(const auto& other : info.farmers)
    {
        if(other.FarmerSort != FARMERTYPE_FARMER || other.SN == g_builderSN) continue;
        if(g_role.count(other.SN) && g_role[other.SN] == ROLE_TMPB) continue;
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

static void orderMove(const tagInfo& info, int sn, double dr, double ur)
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
// 让建筑工继续修建已有建筑。这里不登记为采集目标，避免污染普通村民的资源分配。
static void orderBuilderWork(const tagInfo& info, int sn, int buildingSN)
{
    if(!snFree(sn)) return;
    g_ai->HumanAction(sn, buildingSN);
    markSN(sn);
}
static void orderBuild(const tagInfo& info, int sn, int bType, int bDR, int bUR)
{
    if(!snFree(sn) || g_acceptedBuild.count(sn)) return;
    for(const auto& p : g_pendingBuild) if(p.second.builder == sn) return;
    int order = g_ai->HumanBuild(sn, bType, bDR, bUR);
    g_pendingBuild[order] = {sn, bType, bDR, bUR, g_aiframe};
    markSN(sn);
}
static void orderBldAction(const tagInfo& info, int sn, int act)
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
           building.Type != BUILDING_MARKET && building.Type != BUILDING_RANGE) continue;
        g_loggedCompletedBuildings.insert(building.SN);
        dbg(QString("[建筑完成] type=%1 sn=%2 frame=%3")
                  .arg(building.Type).arg(building.SN).arg(g_aiframe));
    }
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
static int findFreeFarm(const tagInfo& info)
{
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_FARM || b.Percent < 100 || b.Cnt <= 0) continue;
        bool occupied = false;
        for(const auto& f : info.farmers)
            if(f.WorkObjectSN == b.SN) { occupied = true; break; }
        if(!occupied)
            for(const auto& target : g_farmerGatherTargets)
                if(target.second == b.SN) { occupied = true; break; }
        if(!occupied) return b.SN;
    }
    return -1;
}

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
        double d = usrBlockDist(g_defTowerDR, g_defTowerUR, g_tcDR, g_tcUR);
        // 记录塔与TC的距离；第一波仍统一使用这座预置塔防守。
        g_needNewTower = (d > 12.0);
    }
    else
    {
        g_needNewTower = true; // 极端情况：没有预置塔，必须自建
    }

    // 第一波坚持使用预置箭塔：农民集合到塔旁可走平地。
    // 没有预置塔时才退回TC附近作为兜底。
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
    dbg(QString("[防守] 农民集合点=(%1,%2)，防守塔SN=%3")
              .arg(g_rallyDR/BLOCKSIDELENGTH,0,'f',1)
              .arg(g_rallyUR/BLOCKSIDELENGTH,0,'f',1).arg(g_defTowerSN));
}

// 计算祭司/士兵的"塔后安全点"：箭塔朝TC方向后退2格
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
    double L = sqrt(dx*dx + dy*dy);
    if(L < 0.5) { g_priestPointDR = tcx + 2*BLOCKSIDELENGTH; g_priestPointUR = tcy; return; }
    // 从塔向TC方向挪2格
    g_priestPointDR = tx + dx / L * 2.0 * BLOCKSIDELENGTH;
    g_priestPointUR = ty + dy / L * 2.0 * BLOCKSIDELENGTH;
}

// 若补建的第二塔已成型（TC旁8格内出现塔），把主防塔切换为新塔
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
        g_needNewTower = false;
        dbg(QString("[防守] 新箭塔建成，主防塔切换至(%1,%2)")
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

    double tcx = g_tcDR * BLOCKSIDELENGTH, tcy = g_tcUR * BLOCKSIDELENGTH;

    // TC约12格内是否有活体瞪羚：有 => 出双人猎；没有 => 4人全采果
    int gazSN = usrNearestResource(info, RESOURCE_GAZELLE, tcx, tcy, true);
    bool hunt = false;
    if(gazSN != -1)
    {
        const tagResource* gz = getRes(info, gazSN);
        if(gz && usrBlockDist(gz->BlockDR, gz->BlockUR, g_tcDR, g_tcUR) <= 12.0)
            hunt = true;
    }
    g_hunterActive = hunt;

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
    // (3) 4名食物工
    if(hunt)
    {
        g_hunterSN[0] = lands[idx]->SN; assign(lands[idx]->SN, ROLE_HUNTER); idx++;
        g_hunterSN[1] = lands[idx]->SN; assign(lands[idx]->SN, ROLE_HUNTER); idx++;
    }
    for(; idx < 8; idx++)   // 剩余2个（打猎）或4个（不打猎）采果
    {
        assign(lands[idx]->SN, ROLE_BUSH);
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
    if(hunt)
    {
        // 两名猎人同时打同一只瞪羚（不空等，见面2矛秒杀，尸体一起采）
        const tagResource* gz = getRes(info, gazSN);
        if(gz)
        {
            orderAction(info, g_hunterSN[0], gazSN);
            orderAction(info, g_hunterSN[1], gazSN);
        }
        dbg(QString("[开局] 启用双人猎，目标瞪羚@(%1,%2)")
                  .arg(gz?gz->BlockDR:-1).arg(gz?gz->BlockUR:-1));
    }
    else
    {
        dbg(QString("[开局] 附近无近距瞪羚，4人采果"));
    }

    g_initDone = true;
    dbg(QString("[开局] 初始分工完成：1建筑工+3伐木+%1食物（猎人%2）")
              .arg(hunt?4:4).arg(hunt?2:0));
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
        if(b.Type != BUILDING_STOCK || b.Percent <= 0) continue;
        if(usrBlockDist(b.BlockDR, b.BlockUR, g->BlockDR, g->BlockUR) <= 8.0) return true;
    }
    return false;
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
    // 按当前缺口分配，其他村民阵亡后的补员也会恢复目标结构。
    else if(foodN < 6) role = ROLE_BUSH;
    else if(woodN < 10) role = ROLE_WOOD;
    else if(foodN < TARGET_FOOD_WORKERS) role = ROLE_BUSH;
    else role = ROLE_WOOD;

    g_role[sn] = role;

    // 立即派活
    int t = -1;
    if(role == ROLE_WOOD)        t = usrBestGatherResource(info, RESOURCE_TREE, *fp);
    else if(role == ROLE_BUSH)   t = usrBestGatherResource(info, RESOURCE_BUSH, *fp);
    else if(role == ROLE_GOLD)   t = usrBestGatherResource(info, RESOURCE_GOLD, *fp);
    else if(role == ROLE_STONE)  t = usrBestGatherResource(info, RESOURCE_STONE, *fp);
    else if(role == ROLE_FARM)
    {
        t = findFreeFarm(info);                          // 只去没人种的田
        if(t == -1) t = usrBestGatherResource(info, RESOURCE_BUSH, *fp);
    }
    if(t != -1) orderAction(info, sn, t);

    static const char* rn[] = {"无","建筑","伐木","采果","打猎","种田","采金","采石","临建"};
    dbg(QString("[村民分配] farmer=%1 role=%2 target=%3 food=%4 wood=%5")
              .arg(sn).arg(rn[role]).arg(t)
              .arg(foodN+(role==ROLE_BUSH)).arg(woodN+(role==ROLE_WOOD)));
}

// ====================== 7. 双人猎管理（不空等、只猎瞪羚、保持编队） ======================
static void manageHunters(const tagInfo& info)
{
    if(!g_hunterActive) return;

    const tagFarmer* f0 = usrGetFarmer(info, g_hunterSN[0]);
    const tagFarmer* f1 = usrGetFarmer(info, g_hunterSN[1]);
    // 有人阵亡/消失：幸存者转采果，解散编队
    if(!f0 || !f1)
    {
        if(f0) g_role[g_hunterSN[0]] = ROLE_BUSH;
        if(f1) g_role[g_hunterSN[1]] = ROLE_BUSH;
        g_hunterActive = false;
        g_hunterSN[0] = g_hunterSN[1] = -1;
        dbg(QString("[打猎] 编队解散，幸存者转采果"));
        return;
    }

    // 选目标：队长正在打的活体瞪羚优先；否则挑离队长最近、且离TC不超过20格的活体瞪羚
    int prey = -1;
    if(f0->WorkObjectSN > 0)
    {
        const tagResource* w = getRes(info, f0->WorkObjectSN);
        if(w && w->Type == RESOURCE_GAZELLE && w->Blood > 0) prey = w->SN;
    }
    if(prey == -1)
    {
        prey = usrNearestResource(info, RESOURCE_GAZELLE, f0->DR, f0->UR, true);
        if(prey != -1)
        {
            const tagResource* gz = getRes(info, prey);
            if(gz && usrBlockDist(gz->BlockDR, gz->BlockUR, g_tcDR, g_tcUR) > 20.0)
                prey = -1;  // 太远不追，防止满图跑
        }
    }
    // 没有活体瞪羚：就近采瞪羚尸体（Blood==0 也是 RESOURCE_GAZELLE）
    if(prey == -1)
        prey = usrNearestResource(info, RESOURCE_GAZELLE, f0->DR, f0->UR, false);
    // 连尸体都没有：两人转采果，编队解散
    if(prey == -1)
    {
        int b0 = usrNearestResource(info, RESOURCE_BUSH, f0->DR, f0->UR, false);
        int b1 = usrNearestResource(info, RESOURCE_BUSH, f1->DR, f1->UR, false);
        if(b0 != -1) orderAction(info, g_hunterSN[0], b0);
        if(b1 != -1) orderAction(info, g_hunterSN[1], b1);
        g_role[g_hunterSN[0]] = ROLE_BUSH;
        g_role[g_hunterSN[1]] = ROLE_BUSH;
        g_hunterActive = false;
        g_hunterSN[0] = g_hunterSN[1] = -1;
        dbg(QString("[打猎] 瞪羚采光，两人转采果"));
        return;
    }
    // 两人都空闲才下令（队长先打，队友汇合后打同一只；不打断赶路/攻击）
    if(f0->NowState == HUMAN_STATE_IDLE) orderAction(info, g_hunterSN[0], prey);
    if(f1->NowState == HUMAN_STATE_IDLE) orderAction(info, g_hunterSN[1], prey);
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
    orderBuild(info, builderSN, bType, x, y);
    g_lastBuildTry[key] = g_aiframe;
    dbg(QString("[建造] 类型%1 @(%2,%3)").arg(bType).arg(x).arg(y));
    return true;
}

// 专职建筑工的建造链（只在他IDLE时派新活；无活时原地待命，保证随叫随到）
static bool militaryProductionNeeded(const tagInfo& info)
{
    if(info.civilizationStage < CIVILIZATION_BRONZEAGE) return false;
    for(const auto& b : info.buildings)
    {
        if(b.Percent < 100) continue;
        if((b.Type == BUILDING_RANGE || b.Type == BUILDING_STABLE ||
            b.Type == BUILDING_COLLAGE || b.Type == BUILDING_ARMYCAMP) &&
           b.Project != ACT_NULL) return true;
        if(b.Type == BUILDING_RANGE && info.Meat >= COST_BOWMAN_FOOD &&
           (info.Wood >= COST_BOWMAN_WOOD || info.Gold >= COST_COMPOSITE_GOLD)) return true;
        if(b.Type == BUILDING_STABLE && info.Meat >= 70 && info.Gold >= 80) return true;
        if(b.Type == BUILDING_COLLAGE && info.Meat >= 60 && info.Gold >= 40) return true;
    }
    return false;
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
    if(target < 0) target = findFreeFarm(info);
    if(target < 0)
        target = usrNearestResource(info, RESOURCE_GAZELLE,
                                    farmer.DR, farmer.UR, false);
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

// 铜器时代且金矿仓库可用后，从空闲伐木工中逐帧转出两人采金。
static void manageGoldMiners(const tagInfo& info)
{
    if(info.civilizationStage < CIVILIZATION_BRONZEAGE || !stockNearGold(info) ||
       countRole(ROLE_GOLD) >= TARGET_GOLD_MINER) return;

    for(const auto& farmer : info.farmers)
    {
        if(farmer.FarmerSort != FARMERTYPE_FARMER || farmer.SN == g_builderSN ||
           farmer.NowState != HUMAN_STATE_IDLE || !snFree(farmer.SN) ||
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

static void builderQueue(const tagInfo& info)
{
    if(g_builderSN == -1) return;
    const tagFarmer* bf = usrGetFarmer(info, g_builderSN);
    if(!bf) { g_builderSN = -1; return; }
    if(bf->NowState != HUMAN_STATE_IDLE) return;

    // 防守撤退等情况可能中断前置建筑施工。优先回去续建，不能另起一座导致升铜延误。
    for(const auto& building : info.buildings)
    {
        bool upgradePrerequisite = building.Type == BUILDING_ARMYCAMP ||
                                   building.Type == BUILDING_MARKET ||
                                   building.Type == BUILDING_RANGE;
        if(upgradePrerequisite && building.Percent > 0 && building.Percent < 100)
        {
            orderBuilderWork(info, g_builderSN, building.SN);
            dbg(QString("[前置续建] builder=%1 type=%2 sn=%3 percent=%4")
                      .arg(g_builderSN).arg(building.Type).arg(building.SN)
                      .arg(building.Percent));
            return;
        }
    }

    int houses  = usrCountBuilt(info, BUILDING_HOME);
    int camps   = usrCountBuilt(info, BUILDING_ARMYCAMP);
    int markets = usrCountBuilt(info, BUILDING_MARKET);
    int ranges  = usrCountBuilt(info, BUILDING_RANGE);
    int stables = usrCountBuilt(info, BUILDING_STABLE);
    int collages= usrCountBuilt(info, BUILDING_COLLAGE);
    bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;
    bool upgradePrerequisites = markets >= 1 && (ranges >= 1 || stables >= 1);

    int landFarmers = usrCountLandFarmers(info);
    int freePopulation = info.Human_MaxNum - (int)info.Human_Num;
    bool savingForBronze = !bronze && upgradePrerequisites;
    int currentFarmerTarget = bronze ? TARGET_LAND_FARMERS : PRE_BRONZE_LAND_FARMERS;
    bool earlyHouse = !savingForBronze && landFarmers < currentFarmerTarget &&
                      freePopulation <= 2;
    bool armyHouse = bronze && landFarmers >= TARGET_LAND_FARMERS &&
                     freePopulation <= 2 && militaryProductionNeeded(info);
    bool needHouse = houses < MAX_HOUSES && (earlyHouse || armyHouse);

    // (0) 人口房最优先，避免卡人口停产
    if(needHouse && info.Wood >= COST_HOUSE_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_HOME, BUILDING_HOME,
                   g_tcDR, g_tcUR, 3, 10);
        return;
    }
    // (1) 兵营（靶场/马厩前置，也是投石兵训练地）
    if(camps < 1 && info.Wood >= COST_CAMP_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_ARMYCAMP, BUILDING_ARMYCAMP,
                   g_tcDR, g_tcUR, 3, 11);
        return;
    }
    // (2) 市场（升铜前置之一）
    if(markets < 1 && info.Wood >= COST_MARKET_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_MARKET, BUILDING_MARKET,
                   g_tcDR, g_tcUR, 3, 11);
        return;
    }
    // (3) 靶场（升铜前置之二，150木，需兵营）
    if(camps >= 1 && ranges < 1 && info.Wood >= COST_RANGE_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_RANGE, BUILDING_RANGE,
                   g_tcDR, g_tcUR, 3, 11);
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
                       g_tcDR, g_tcUR, 3, 9);
            return;
        }
    }
    // 前置建筑完成后为800食物升级留资源，铜器时代前不再插入其他建筑。
    if(!bronze) return;

    if(bronze)
    {
        // 第二座箭塔、猎场仓库都推迟到铜器时代，不能延误升级前置。
        if(g_needNewTower && g_towerTechDone && info.Stone >= COST_TOWER_STONE)
        {
            double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
            bool nearTower = false;
            for(const auto& b : info.buildings)
                if(b.Type==BUILDING_ARROWTOWER && b.Percent>0 &&
                   d2(tcx,tcy,b.BlockDR*BLOCKSIDELENGTH,b.BlockUR*BLOCKSIDELENGTH)
                   < 8*8*BLOCKSIDELENGTH*BLOCKSIDELENGTH) { nearTower = true; break; }
            if(!nearTower)
            {
                tryBuildAt(info, g_builderSN, BUILDING_ARROWTOWER, BUILDING_ARROWTOWER,
                           g_tcDR, g_tcUR, 3, 7);
                return;
            }
        }
        if(g_hunterActive && !g_huntStockBuilt && info.Wood >= COST_STOCK_WOOD)
        {
            const tagFarmer* hunter = usrGetFarmer(info, g_hunterSN[0]);
            int preySN = hunter ? usrNearestResource(info, RESOURCE_GAZELLE,
                                                     hunter->DR, hunter->UR, false) : -1;
            const tagResource* prey = preySN >= 0 ? getRes(info, preySN) : nullptr;
            if(prey)
            {
                bool closeStock = false;
                for(const auto& b : info.buildings)
                    if(b.Type==BUILDING_STOCK && b.Percent>0 &&
                       usrBlockDist(b.BlockDR,b.BlockUR,prey->BlockDR,prey->BlockUR) <= 8.0)
                    { closeStock = true; break; }
                if(closeStock) g_huntStockBuilt = true;
                else
                {
                    tryBuildAt(info, g_builderSN, BUILDING_STOCK, KEY_STOCK_HUNT,
                               prey->BlockDR, prey->BlockUR, 2, 6);
                    return;
                }
            }
        }
        // 金矿旁仓库（采金前置）
        if(!g_goldStockBuilt && info.Wood >= COST_STOCK_WOOD)
        {
            double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
            int goldSN = usrNearestResource(info, RESOURCE_GOLD, tcx, tcy, false);
            const tagResource* g = goldSN>=0 ? getRes(info, goldSN) : nullptr;
            if(g)
            {
                bool closeStock = false;
                for(const auto& b : info.buildings)
                    if(b.Type==BUILDING_STOCK && b.Percent>0 &&
                       usrBlockDist(b.BlockDR,b.BlockUR,g->BlockDR,g->BlockUR) <= 8.0)
                    { closeStock = true; break; }
                if(closeStock) g_goldStockBuilt = true;
                else
                {
                    tryBuildAt(info, g_builderSN, BUILDING_STOCK, KEY_STOCK_GOLD,
                               g->BlockDR, g->BlockUR, 2, 6);
                    return;
                }
            }
        }
        // 马厩、学院（第二波兵种建筑）
        if(camps >= 1 && stables < 1 && info.Wood >= COST_STABLE_WOOD)
        {
            tryBuildAt(info, g_builderSN, BUILDING_STABLE, BUILDING_STABLE,
                       g_tcDR, g_tcUR, 3, 12);
            return;
        }
        if(stables >= 1 && collages < 1 && info.Wood >= COST_COLLAGE_WOOD)
        {
            tryBuildAt(info, g_builderSN, BUILDING_COLLAGE, BUILDING_COLLAGE,
                       g_tcDR, g_tcUR, 3, 12);
            return;
        }
    }

}

// 农田并行建造：市场建成后，从未分配的空闲村民里抽最多3人当临时建筑工，
// 每人开一片农田；建完(IDLE)自动转为 ROLE_FARM 去种地
static void manageFarmBuilders(const tagInfo& info)
{
    if(info.civilizationStage < CIVILIZATION_BRONZEAGE) return;
    if(usrCountBuilt(info, BUILDING_MARKET) < 1) return;

    int farmTotal = 0;
    for(const auto& b : info.buildings)
        if(b.Type == BUILDING_FARM && b.Percent > 0) farmTotal++;

    // 清理已消失的临时建筑工
    for(auto it = g_tmpBuilders.begin(); it != g_tmpBuilders.end(); )
    {
        if(!usrGetFarmer(info, *it)) it = g_tmpBuilders.erase(it);
        else ++it;
    }

    // 还没建够：抽新的空闲无岗位村民开建
    if(farmTotal + (int)g_tmpBuilders.size() < TARGET_FARM_NUM && info.Wood >= COST_FARM_WOOD)
    {
        for(const auto& f : info.farmers)
        {
            if(f.FarmerSort != FARMERTYPE_FARMER) continue;
            if(f.NowState != HUMAN_STATE_IDLE) continue;
            auto it = g_role.find(f.SN);
            if(it != g_role.end() && it->second != ROLE_NONE) continue; // 只要无岗位的
            if(farmTotal + (int)g_tmpBuilders.size() >= TARGET_FARM_NUM) break;
            if(info.Wood < COST_FARM_WOOD) break;

            int x, y;
            if(usrFindFlatNear(x, y, info, g_tcDR, g_tcUR, 3, 9))
            {
                orderBuild(info, f.SN, BUILDING_FARM, x, y);
                g_role[f.SN] = ROLE_TMPB;
                g_tmpBuilders.insert(f.SN);
                g_tmpRetry[f.SN] = g_aiframe;
                farmTotal++;
                dbg(QString("[农田] 临时建筑工%1 开建农田@(%2,%3)，当前%4片")
                          .arg(f.SN).arg(x).arg(y).arg(farmTotal));
            }
            break; // 每帧最多新开1片，稳一点
        }
    }

    // 临时建筑工状态跟踪：IDLE 说明这片建完了（或下单失败需重试）
    for(auto it = g_tmpBuilders.begin(); it != g_tmpBuilders.end(); )
    {
        int sn = *it;
        const tagFarmer* fp = usrGetFarmer(info, sn);
        if(!fp) { it = g_tmpBuilders.erase(it); continue; }
        if(fp->NowState != HUMAN_STATE_IDLE) { ++it; continue; }

        // 已有可种农田 => 转正式农民
        bool hasFarm = false;
        for(const auto& b : info.buildings)
            if(b.Type==BUILDING_FARM && b.Percent>=100 && b.Cnt>0) { hasFarm = true; break; }
        if(hasFarm)
        {
            g_role[sn] = ROLE_FARM;
            it = g_tmpBuilders.erase(it);
            int t = findFreeFarm(info);
            if(t != -1) orderAction(info, sn, t);
            dbg(QString("[农田] 村民%1 建完转种田").arg(sn));
            continue;
        }
        // 没建起来：90帧节流重试一次
        if(g_aiframe - g_tmpRetry[sn] >= 90)
        {
            int x, y;
            if(usrFindFlatNear(x, y, info, g_tcDR, g_tcUR, 3, 9) && info.Wood >= COST_FARM_WOOD)
            {
                orderBuild(info, sn, BUILDING_FARM, x, y);
                g_tmpRetry[sn] = g_aiframe;
            }
        }
        ++it;
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
    const int farmerTarget = info.civilizationStage >= CIVILIZATION_BRONZEAGE
                             ? TARGET_LAND_FARMERS : PRE_BRONZE_LAND_FARMERS;
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
            bool savingForBronze = info.civilizationStage == CIVILIZATION_TOOLAGE && pre2;
            if(info.civilizationStage == CIVILIZATION_TOOLAGE
               && info.Meat >= COST_BRONZE_FOOD && pre2)
            {
                orderBldAction(info, b.SN, BUILDING_CENTER_UPGRADE);
                dbg(QString("[升级] 点击升铜器时代（800食物/60秒）"));
            }
            // 否则不停造村民（50食/20秒；人口满或食物不足时内核会拒，这里先做门限）
            else if(!savingForBronze && landFarmers < farmerTarget &&
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
                orderBldAction(info, b.SN, BUILDING_GRANARY_ARROWTOWER);
                g_towerTechIssued = true;
                dbg(QString("[科技] 谷仓研发箭塔科技"));
            }
        }
        // —— 兵营：第一波前出2个投石兵补伤害（仅在不补塔、石头留着没用时）——
        else if(b.Type == BUILDING_ARMYCAMP)
        {
            if(!g_needNewTower)
            {
                int sling = 0;
                for(const auto& a : info.armies) if(a.Sort == AT_SLINGER) sling++;
                // 4:30前训练即可；食物留110（够2个村民），石头10/个
                if(sling < TARGET_SLINGER && g_aiframe < WAVE1_FRAME + SEC(30)
                   && info.Meat >= 150 && info.Stone >= COST_SLINGER_STONE)
                {
                    orderBldAction(info, b.SN, BUILDING_ARMYCAMP_CREATE_SLINGER);
                }
            }
        }
        // —— 市场：伐木科技（铜器后，120食75木） ——
        else if(b.Type == BUILDING_MARKET)
        {
            if(info.civilizationStage >= CIVILIZATION_BRONZEAGE
               && !g_woodTechIssued && info.Meat >= COST_TECH_WOOD_F && info.Wood >= COST_TECH_WOOD_W)
            {
                orderBldAction(info, b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                g_woodTechIssued = true;
                dbg(QString("[科技] 市场研发伐木科技"));
            }
        }
        // —— 靶场：复合弓科技(铜器/180食100木)；之后优先复合弓兵，缺金先出普通弓手 ——
        else if(b.Type == BUILDING_RANGE)
        {
            if(info.civilizationStage >= CIVILIZATION_BRONZEAGE
               && !g_compositeIssued && info.Meat >= COST_TECH_COMP_F && info.Wood >= COST_TECH_COMP_W)
            {
                orderBldAction(info, b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
                g_compositeIssued = true;
                dbg(QString("[科技] 靶场研发复合弓科技"));
            }
            else if(info.civilizationStage >= CIVILIZATION_BRONZEAGE)
            {
                if(g_compositeIssued && info.Meat >= COST_COMPOSITE_FOOD && info.Gold >= COST_COMPOSITE_GOLD)
                    orderBldAction(info, b.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
                else if(!g_compositeIssued && info.Meat >= COST_BOWMAN_FOOD && info.Wood >= COST_BOWMAN_WOOD)
                    orderBldAction(info, b.SN, BUILDING_RANGE_CREATE_BOWMAN);
            }
        }
        // —— 马厩：骑兵（70食80金） ——
        else if(b.Type == BUILDING_STABLE && info.civilizationStage >= CIVILIZATION_BRONZEAGE)
        {
            if(info.Meat >= 70 && info.Gold >= 80)
                orderBldAction(info, b.SN, BUILDING_STABLE_CREATE_CAVALRY);
        }
        // —— 学院：方阵兵（60食40金） ——
        else if(b.Type == BUILDING_COLLAGE && info.civilizationStage >= CIVILIZATION_BRONZEAGE)
        {
            if(info.Meat >= 60 && info.Gold >= 40)
                orderBldAction(info, b.SN, BUILDING_COLLAGE_CREATE_HOPLITE);
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
                dbg(QString("[科技] 箭塔科技完成，可以补建箭塔"));
            }
        }
    }
}

// ====================== 10. 祭司：前期有限探路，后期塔后转换 ======================
static void managePriest(const tagInfo& info)
{
    g_priestSN = -1;
    for(const auto& a : info.armies)
        if(a.Sort == AT_PRIEST) { g_priestSN = a.SN; break; }
    if(g_priestSN == -1) return;
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
        // —— 防守期：先站到塔后安全点 ——
        if(pd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH
           && p->NowState != HUMAN_STATE_ATTACKING
           && g_aiframe - g_priestLastMoveFrame >= 200)
        {
            orderMove(info, g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_aiframe;
        }

        // 塔是否已经开火（Project=当前攻击目标SN；ACT_NULL=空闲）
        bool towerFiring = false;
        if(g_defTowerSN != -1)
        {
            const tagBuilding* tw = getBuilding(info, g_defTowerSN);
            if(tw && tw->Project != ACT_NULL) towerFiring = true;
            // 或者敌人已经走到塔射程(7格)内
            double tx = g_defTowerDR*BLOCKSIDELENGTH, ty = g_defTowerUR*BLOCKSIDELENGTH;
            for(const auto& e : info.enemy_armies)
                if(d2(tx,ty,e.DR,e.UR) < 7.0*7.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
                { towerFiring = true; break; }
        }

        // 正在转换中（ATTACKING）不重复下令，等它念完
        if(p->NowState == HUMAN_STATE_ATTACKING) return;
        // 冷却没好不能转
        if(p->ConvertCooldown != 0) return;
        // 必须等塔先拉到仇恨，祭司才出手，避免敌人转头追祭司
        if(!towerFiring) return;

        // 转换射程12格；优先弓箭手（AT_BOWMAN=2），其次最近的敌兵
        int target = -1;
        double range12 = 12.0*12.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH;
        double best = 1e30;
        for(const auto& e : info.enemy_armies)
        {
            double dd = d2(p->DR, p->UR, e.DR, e.UR);
            if(dd > range12) continue;
            if(e.Sort == AT_BOWMAN && dd < best) { best = dd; target = e.SN; }
        }
        if(target == -1)
        {
            for(const auto& e : info.enemy_armies)
            {
                double dd = d2(p->DR, p->UR, e.DR, e.UR);
                if(dd < range12 && dd < best) { best = dd; target = e.SN; }
            }
        }
        if(target != -1)
        {
            orderAction(info, g_priestSN, target);
            g_priestConvertTarget = target;
            dbg(QString("[祭司] 塔已开火，转换敌兵%1").arg(target));
        }
        return;
    }

    // —— 非防守期 ——
    // 3:20~第一波结束：回塔后待命；第一波打完后重新外出扩大探路（找金矿），
    // 但8:00(第二波9:00出发前)必须再次回家；任何时候附近撞见敌人立刻回家
    bool goHome = nearbyEnemy != -1
                  || (g_aiframe >= FRAME_PRIEST_RETURN &&
                      (!g_wave1Handled || g_aiframe >= SEC(480)));
    if(goHome)
    {
        if(pd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH
           && g_aiframe - g_priestLastMoveFrame >= 300)
        {
            orderMove(info, g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_aiframe;
        }
        return;
    }

    // 前期探路：只在TC半径15格内沿8个方向轮流走（既开视野找资源，又不离家太远）
    static const int dx8[8] = {0, 1, 1, 1, 0,-1,-1,-1};
    static const int dy8[8] = {1, 1, 0,-1,-1,-1, 0, 1};
    double tcx = g_tcDR*BLOCKSIDELENGTH, tcy = g_tcUR*BLOCKSIDELENGTH;
    double scoutR = g_wave1Handled ? 22.0 : 15.0;   // 第一波后扩大探路半径找金矿
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
        orderMove(info, g_priestSN, tx, ty);
        g_priestLastMoveFrame = g_aiframe;
        g_priestScoutIdx++;
    }
}

// ====================== 11. 防守状态机 + 农民撤退 ======================
// 返回离防守核心最近的敌兵距离（块），无敌兵返回1e9
static double nearestEnemyBlockDist(const tagInfo& info)
{
    int cDR = (g_defTowerSN != -1) ? g_defTowerDR : g_tcDR;
    int cUR = (g_defTowerSN != -1) ? g_defTowerUR : g_tcUR;
    double best = 1e9;
    for(const auto& e : info.enemy_armies)
    {
        double d = usrBlockDist((int)floor(e.DR/BLOCKSIDELENGTH+0.5),
                                (int)floor(e.UR/BLOCKSIDELENGTH+0.5), cDR, cUR);
        if(d < best) best = d;
    }
    return best;
}

static void updateDefense(const tagInfo& info)
{
    double nd = nearestEnemyBlockDist(info);

    if(!g_defenseMode)
    {
        // 敌兵进入20格时防守；4:40保险条件只允许触发尚未处理的第一波。
        bool enemyNear = g_aiframe >= FRAME_WATCH && nd <= 20.0;
        bool firstWaveDeadline = !g_wave1Handled && g_aiframe >= FRAME_HARD_DEFENSE;
        if(enemyNear || firstWaveDeadline)
        {
            g_defenseMode = true;
            g_calmFrames = 0;
            dbg(QString("[防守] 敌军接近(最近敌兵%1格)，农民撤到箭塔附近!")
                      .arg(nd<1e8?QString::number(nd,'f',1):QString("?")));
        }
    }
    else
    {
        // 敌人退到22格外（或看不见）持续600帧 => 解防复工
        if(nd > 22.0) g_calmFrames++;
        else g_calmFrames = 0;
        if(g_calmFrames >= 600)
        {
            g_defenseMode = false;
            g_wave1Handled = true;
            g_calmFrames = 0;
            dbg(QString("[防守] 第一波已全歼，农民复工"));
        }
    }

    // 防守期：所有陆地农民撤到预置箭塔旁抱团（250帧刷新一次走位）
    if(g_defenseMode)
    {
        for(const auto& f : info.farmers)
        {
            if(f.FarmerSort != FARMERTYPE_FARMER) continue;
            // 升铜前置建筑一旦开工就让专职建筑工完成；撤退命令会取消施工，
            // 造成食物已到800却因靶场未完成而无法升级。
            if(f.SN == g_builderSN)
            {
                const tagBuilding* work = getBuilding(info, f.WorkObjectSN);
                if(work && work->Percent > 0 && work->Percent < 100 &&
                   (work->Type == BUILDING_ARMYCAMP || work->Type == BUILDING_MARKET ||
                    work->Type == BUILDING_RANGE))
                    continue;
            }
            double dd = d2(f.DR, f.UR, g_rallyDR, g_rallyUR);
            if(dd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
            {
                static unordered_map<int,int> lastRetreat;
                auto it = lastRetreat.find(f.SN);
                int last = (it==lastRetreat.end()) ? -99999 : it->second;
                if(g_aiframe - last >= 250)
                {
                    orderMove(info, f.SN, g_rallyDR, g_rallyUR);
                    lastRetreat[f.SN] = g_aiframe;
                }
            }
        }
    }
}

// ====================== 12. 我方士兵：防守期集火，平时集结塔下 ======================
static void manageSoldiers(const tagInfo& info)
{
    int cDR = (g_defTowerSN != -1) ? g_defTowerDR : g_tcDR;
    int cUR = (g_defTowerSN != -1) ? g_defTowerUR : g_tcUR;
    double cx = cDR*BLOCKSIDELENGTH, cy = cUR*BLOCKSIDELENGTH;
    static unordered_map<int,int> lastRally;

    for(const auto& a : info.armies)
    {
        if(a.Sort == AT_PRIEST) continue;   // 祭司单独管

        if(g_defenseMode)
        {
            // 找离防守核心11格内的敌兵；优先弓手
            int target = -1;
            double best = 1e30;
            double r11 = 11.0*11.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH;
            for(const auto& e : info.enemy_armies)
            {
                double dc = d2(cx, cy, e.DR, e.UR);
                if(dc > r11) continue;
                if(e.Sort == AT_BOWMAN && dc < best) { best = dc; target = e.SN; }
            }
            if(target == -1)
            {
                for(const auto& e : info.enemy_armies)
                {
                    double dc = d2(cx, cy, e.DR, e.UR);
                    if(dc < r11 && dc < best) { best = dc; target = e.SN; }
                }
            }
            // 空闲/赶路时下令；正在攻击则不打断
            if(target != -1 && a.NowState != HUMAN_STATE_ATTACKING)
                orderAction(info, a.SN, target);
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
                    orderMove(info, a.SN, g_priestPointDR, g_priestPointUR);
                    lastRally[a.SN] = g_aiframe;
                }
            }
        }
    }
}

// ====================== 13. 空闲村民按岗位续任务（防挂机） ======================
static void idleResume(const tagInfo& info)
{
    // 防守期农民在撤退，不派生产活
    if(g_defenseMode) return;

    for(auto& kv : g_role)
    {
        int sn = kv.first, role = kv.second;
        const tagFarmer* f = usrGetFarmer(info, sn);
        if(!f) { kv.second = ROLE_NONE; continue; }     // 人没了，清岗位
        if(f->NowState != HUMAN_STATE_IDLE) continue;

        int t = -1;
        if(role == ROLE_WOOD)
            t = usrBestGatherResource(info, RESOURCE_TREE, *f);
        else if(role == ROLE_BUSH)
        {
            t = usrBestGatherResource(info, RESOURCE_BUSH, *f);
            if(t == -1) // 浆果采光：种空闲田，再不行采瞪羚
            {
                t = findFreeFarm(info);
                if(t == -1) t = usrNearestResource(info, RESOURCE_GAZELLE, f->DR, f->UR, false);
            }
        }
        else if(role == ROLE_FARM)
        {
            t = findFreeFarm(info);
            if(t == -1) t = usrBestGatherResource(info, RESOURCE_BUSH, *f);
        }
        else if(role == ROLE_GOLD)
            t = usrBestGatherResource(info, RESOURCE_GOLD, *f);
        else if(role == ROLE_STONE)
            t = usrBestGatherResource(info, RESOURCE_STONE, *f);
        // ROLE_BUILDER 由 builderQueue 接管；ROLE_TMPB 由 manageFarmBuilders 接管；
        // ROLE_HUNTER 由 manageHunters 接管
        if(t != -1) orderAction(info, sn, t);
    }
}

// 农田采完(Cnt<=0)补种：派空闲村民去重整该农田
static void replantFarms(const tagInfo& info)
{
    if(g_defenseMode) return;
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_FARM || b.Percent < 100 || b.Cnt > 0) continue;
        for(const auto& f : info.farmers)
        {
            if(f.FarmerSort != FARMERTYPE_FARMER || f.NowState != HUMAN_STATE_IDLE) continue;
            if(f.SN == g_builderSN) continue;
            auto it = g_role.find(f.SN);
            int role = (it==g_role.end()) ? ROLE_NONE : it->second;
            if(role == ROLE_TMPB) continue;
            orderAction(info, f.SN, b.SN);
            break;
        }
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

    // 3) 建造链（专职建筑工 + 农田并行建设）
    //    防守期暂停外派建造，建筑工随农民一起撤到TC旁，避免外出送命
    if(!g_defenseMode)
    {
        builderQueue(info);
        manageFarmBuilders(info);
    }

    // 4) 新村民分配（每帧最多1个）；防守期暂缓，新村民留在TC旁
    if(g_initDone && !g_defenseMode) assignNewFarmer(info);

    // 升铜前临时提高食物工比例，铜器完成后恢复原有经济结构。
    if(g_initDone && !g_defenseMode) manageUpgradeFoodWorkers(info);

    // 铜器时代且金矿仓库可用后，将两个空闲伐木工转为采金。
    if(g_initDone && !g_defenseMode) manageGoldMiners(info);

    // 5) 双人猎
    if(g_initDone && !g_defenseMode) manageHunters(info);

    // 6) 防守状态机（农民撤退）
    updateDefense(info);

    // 7) 祭司（探路/回防/转换）
    managePriest(info);

    // 8) 士兵集结/集火
    manageSoldiers(info);

    // 9) 空闲续任务 + 农田补种
    idleResume(info);
    replantFarms(info);
}

