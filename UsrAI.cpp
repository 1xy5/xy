#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

// ====================== 1. 本 AI 自用常量（口径=config.json） ======================
static const int FPS = 25;
static inline int SEC(int s) { return s * FPS; }   // 秒 -> 帧

static const int FRAME_PRIEST_RETURN = SEC(200);   // 3:20 祭司停止探路回防
static const int FRAME_WATCH         = SEC(220);   // 3:40 起进入敌情观察期
static const int FRAME_HARD_DEFENSE  = SEC(280);   // 4:40 保险：无论是否看见敌人都进入防守
static const int WAVE1_FRAME         = 6000;       // 第一波出发帧（源码 FAT）

static const int COST_FARMER_FOOD   = 50;
static const int COST_BRONZE_FOOD   = 800;
static const int COST_HOUSE_WOOD    = 30;
static const int COST_STOCK_WOOD    = 120;
static const int COST_CAMP_WOOD     = 125;
static const int COST_MARKET_WOOD   = 150;
static const int COST_RANGE_WOOD    = 150;
static const int COST_STABLE_WOOD   = 150;
static const int COST_COLLAGE_WOOD  = 180;
static const int COST_FARM_WOOD     = 75;
static const int COST_TOWER_STONE   = 150;
static const int COST_TOWERTECH_FOOD= 50;
static const int COST_SLINGER_FOOD  = 40;
static const int COST_SLINGER_STONE= 10;
static const int COST_BOWMAN_FOOD   = 40;
static const int COST_BOWMAN_WOOD   = 20;
static const int COST_COMPOSITE_FOOD= 40;
static const int COST_COMPOSITE_GOLD= 20;
static const int COST_TECH_WOOD_F   = 120;
static const int COST_TECH_WOOD_W   = 75;
static const int COST_TECH_COMP_F   = 180;
static const int COST_TECH_COMP_W   = 100;

static const int TARGET_FARM_NUM    = 6;    // 农田目标片数（每片250食物）
static const int TARGET_SLINGER     = 2;    // 不补塔时训练2个投石兵补伤害
static const int TARGET_GOLD_MINER  = 2;    // 铜器后采金人数

// 建造去重的伪类型 key（仓库有两个不同选址，用不同 key 分别节流）
static const int KEY_STOCK_HUNT = 9001;
static const int KEY_STOCK_GOLD = 9002;

// ====================== 2. 全局状态变量定义 ======================
int  g_aiframe = 0;

unordered_map<int,int> g_role;
int  g_builderSN = -1;
int  g_hunterSN[2] = {-1, -1};
bool g_hunterActive = false;
int  g_newFarmerIdx = 0;
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

// 发包辅助函数是文件级自由函数，不能直接调 AI 的成员函数，
// 用 processData 开头保存的 this 指针转发
static AI* g_ai = nullptr;
static void dbg(const QString& s) { if(g_ai) g_ai->DebugText(s); }

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

    static const int dx8[8] = {1, 1, 0,-1,-1,-1, 0, 1};
    static const int dy8[8] = {0, 1, 1, 1, 0,-1,-1,-1};
    static int s_rot = 0;
    int startDir = s_rot;
    s_rot = (s_rot + 1) % 8;

    for(int r = minR; r <= maxR; r++)
    {
        for(int k = 0; k < 8; k++)
        {
            int dir = (startDir + k) % 8;
            int dr = cDR + dx8[dir] * r;
            int ur = cUR + dy8[dir] * r;
            if(dr < 2 || ur < 2 || dr >= rows-2 || ur >= cols-2) continue;

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
        double d = d2(fromDR, fromUR, r.DR, r.UR);
        if(d < bd) { bd = d; best = r.SN; }
    }
    return best;
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
    g_ai->HumanAction(sn, objSN);
    markSN(sn);
}
static void orderBuild(const tagInfo& info, int sn, int bType, int bDR, int bUR)
{
    if(!snFree(sn)) return;
    g_ai->HumanBuild(sn, bType, bDR, bUR);
    markSN(sn);
}
static void orderBldAction(const tagInfo& info, int sn, int act)
{
    if(!snFree(sn)) return;
    g_ai->BuildingAction(sn, act);
    markSN(sn);
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
        // 预置塔离TC超过12格（map2约19格），第一波赶不回来，必须在TC旁补第二塔
        g_needNewTower = (d > 12.0);
    }
    else
    {
        g_needNewTower = true; // 极端情况：没有预置塔，必须自建
    }

    // 农民抱团点：TC旁2格（findFlatNear保证可走）
    int rx, ry;
    if(usrFindFlatNear(rx, ry, info, g_tcDR, g_tcUR, 2, 4))
    {
        g_rallyDR = rx * BLOCKSIDELENGTH;
        g_rallyUR = ry * BLOCKSIDELENGTH;
    }
    else
    {
        g_rallyDR = tcx + 2*BLOCKSIDELENGTH;
        g_rallyUR = tcy + 2*BLOCKSIDELENGTH;
    }
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
            int t = usrNearestResource(info, RESOURCE_TREE, f.DR, f.UR, false);
            if(t != -1) orderAction(info, f.SN, t);
        }
        else if(role == ROLE_BUSH)
        {
            int t = usrNearestResource(info, RESOURCE_BUSH, f.DR, f.UR, false);
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
    bool marketReady = usrCountBuilt(info, BUILDING_MARKET) >= 1;
    bool bronzeReached = info.civilizationStage >= CIVILIZATION_BRONZEAGE;

    if(g_newFarmerIdx <= 1)
    {
        role = ROLE_BUSH;                    // #0 #1 补采果，食物工凑到6
    }
    else if(g_newFarmerIdx == 2)
    {
        role = ROLE_WOOD;                    // #2 补第4个伐木
    }
    else
    {
        // #3 起：铜器且金矿仓库就绪 -> 采金；市场好了 -> 种田；
        // 否则继续补食物到7人，再补伐木到6人，最后兜底种田
        if(bronzeReached && stockNearGold(info) && countRole(ROLE_GOLD) < TARGET_GOLD_MINER)
            role = ROLE_GOLD;
        else if(marketReady)
            role = ROLE_FARM;
        else if(foodN < 7)
            role = ROLE_BUSH;
        else if(woodN < 6)
            role = ROLE_WOOD;
        else
            role = ROLE_FARM;
    }

    g_role[sn] = role;
    g_newFarmerIdx++;

    // 立即派活
    int t = -1;
    if(role == ROLE_WOOD)        t = usrNearestResource(info, RESOURCE_TREE,  fp->DR, fp->UR, false);
    else if(role == ROLE_BUSH)   t = usrNearestResource(info, RESOURCE_BUSH,  fp->DR, fp->UR, false);
    else if(role == ROLE_GOLD)   t = usrNearestResource(info, RESOURCE_GOLD,  fp->DR, fp->UR, false);
    else if(role == ROLE_STONE)  t = usrNearestResource(info, RESOURCE_STONE, fp->DR, fp->UR, false);
    else if(role == ROLE_FARM)
    {
        t = findFreeFarm(info);                          // 只去没人种的田
        if(t == -1) t = usrNearestResource(info, RESOURCE_BUSH, fp->DR, fp->UR, false); // 田没好先采果
    }
    if(t != -1) orderAction(info, sn, t);

    static const char* rn[] = {"无","建筑","伐木","采果","打猎","种田","采金","采石","临建"};
    dbg(QString("[分配] 新村民#%1 -> %2").arg(g_newFarmerIdx-1).arg(rn[role]));
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
    if(g_frame - lastTry(key) < 45) return false;
    int x, y;
    if(!usrFindFlatNear(x, y, info, cDR, cUR, minR, maxR))
    {
        g_lastBuildTry[key] = g_frame;   // 没找到地也算一次尝试，避免每帧刷屏
        return false;
    }
    orderBuild(info, builderSN, bType, x, y);
    g_lastBuildTry[key] = g_frame;
    dbg(QString("[建造] 类型%1 @(%2,%3)").arg(bType).arg(x).arg(y));
    return true;
}

// 专职建筑工的建造链（只在他IDLE时派新活；无活时原地待命，保证随叫随到）
static void builderQueue(const tagInfo& info)
{
    if(g_builderSN == -1) return;
    const tagFarmer* bf = usrGetFarmer(info, g_builderSN);
    if(!bf) { g_builderSN = -1; return; }
    if(bf->NowState != HUMAN_STATE_IDLE) return;

    int houses  = usrCountBuilt(info, BUILDING_HOME);
    int camps   = usrCountBuilt(info, BUILDING_ARMYCAMP);
    int markets = usrCountBuilt(info, BUILDING_MARKET);
    int ranges  = usrCountBuilt(info, BUILDING_RANGE);
    int stables = usrCountBuilt(info, BUILDING_STABLE);
    int collages= usrCountBuilt(info, BUILDING_COLLAGE);
    bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;

    // 人口余量：上限-当前 <= 1.5 就要提前补房（每房+4人口，最多12座）
    bool needHouse = (info.Human_MaxNum - info.Human_Num <= 1.5) && houses < 12;

    // (0) 人口房最优先，避免卡人口停产
    if(needHouse && info.Wood >= COST_HOUSE_WOOD)
    {
        if(!usrHasBuilding(info, BUILDING_HOME) || houses < 12)
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
    // (2) TC旁补第二箭塔（仅预置塔太远时；要等箭塔科技完成）
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
    // (3) 市场（升铜前置之一，且解锁农田）
    if(markets < 1 && info.Wood >= COST_MARKET_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_MARKET, BUILDING_MARKET,
                   g_tcDR, g_tcUR, 3, 11);
        return;
    }
    // (4) 猎场太远时，在瞪羚旁边补一座仓库（肉少跑路；只建1座）
    if(g_hunterActive && !g_huntStockBuilt && info.Wood >= COST_STOCK_WOOD)
    {
        const tagFarmer* f0 = usrGetFarmer(info, g_hunterSN[0]);
        if(f0)
        {
            int preySN = usrNearestResource(info, RESOURCE_GAZELLE, f0->DR, f0->UR, false);
            const tagResource* pr = preySN>=0 ? getRes(info, preySN) : nullptr;
            if(pr)
            {
                bool closeStock = false;
                for(const auto& b : info.buildings)
                    if(b.Type==BUILDING_STOCK && b.Percent>0 &&
                       usrBlockDist(b.BlockDR,b.BlockUR,pr->BlockDR,pr->BlockUR) <= 10.0)
                    { closeStock = true; break; }
                if(closeStock) g_huntStockBuilt = true;
                else
                {
                    tryBuildAt(info, g_builderSN, BUILDING_STOCK, KEY_STOCK_HUNT,
                               pr->BlockDR, pr->BlockUR, 2, 6);
                    return;
                }
            }
        }
    }
    // (5) 靶场（升铜前置之二，150木，需兵营）
    if(camps >= 1 && ranges < 1 && info.Wood >= COST_RANGE_WOOD)
    {
        tryBuildAt(info, g_builderSN, BUILDING_RANGE, BUILDING_RANGE,
                   g_tcDR, g_tcUR, 3, 11);
        return;
    }
    // (6) 农田由"临时建筑工并行建"，专职builder不参与（见 manageFarmBuilders）

    // ---- 以下为铜器后为第二波铺垫（不影响第一波/升铜主线） ----
    if(bronze)
    {
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

    // (7) 有余木继续补房，把人口上限往50推
    if(houses < 12 && info.Wood >= COST_HOUSE_WOOD)
        tryBuildAt(info, g_builderSN, BUILDING_HOME, BUILDING_HOME,
                   g_tcDR, g_tcUR, 3, 12);
}

// 农田并行建造：市场建成后，从未分配的空闲村民里抽最多3人当临时建筑工，
// 每人开一片农田；建完(IDLE)自动转为 ROLE_FARM 去种地
static void manageFarmBuilders(const tagInfo& info)
{
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
                g_tmpRetry[f.SN] = g_frame;
                farmTotal++;
                dbg(QString("[农田] 临时建筑工%1 开建农田@(%2,%3)，当前%4片")
                          .arg(f.SN).arg(x).arg(y).arg(farmTotal));
            }
            break; // 每帧最多新开1片，稳一点
        }
    }

    // 临时建筑工状态跟踪：IDLE 说明这片建完了（或下单失败需重试）
    for(int sn : g_tmpBuilders)
    {
        const tagFarmer* fp = usrGetFarmer(info, sn);
        if(!fp) continue;
        if(fp->NowState != HUMAN_STATE_IDLE) continue;

        // 已有可种农田 => 转正式农民
        bool hasFarm = false;
        for(const auto& b : info.buildings)
            if(b.Type==BUILDING_FARM && b.Percent>=100 && b.Cnt>0) { hasFarm = true; break; }
        if(hasFarm)
        {
            g_role[sn] = ROLE_FARM;
            g_tmpBuilders.erase(sn);
            int t = findFreeFarm(info);
            if(t != -1) orderAction(info, sn, t);
            dbg(QString("[农田] 村民%1 建完转种田").arg(sn));
            continue;
        }
        // 没建起来：90帧节流重试一次
        if(g_frame - g_tmpRetry[sn] >= 90)
        {
            int x, y;
            if(usrFindFlatNear(x, y, info, g_tcDR, g_tcUR, 3, 9) && info.Wood >= COST_FARM_WOOD)
            {
                orderBuild(info, sn, BUILDING_FARM, x, y);
                g_tmpRetry[sn] = g_frame;
            }
        }
    }
}

// ====================== 9. 科技 / 升时代 / 造兵 ======================
static void techAndProduction(const tagInfo& info)
{
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
            if(info.civilizationStage == CIVILIZATION_TOOLAGE
               && info.Meat >= COST_BRONZE_FOOD && pre2)
            {
                orderBldAction(info, b.SN, BUILDING_CENTER_UPGRADE);
                dbg(QString("[升级] 点击升铜器时代（800食物/60秒）"));
            }
            // 否则不停造村民（50食/20秒；人口满或食物不足时内核会拒，这里先做门限）
            else if(info.Human_Num < info.Human_MaxNum && info.Meat >= COST_FARMER_FOOD)
                orderBldAction(info, b.SN, BUILDING_CENTER_CREATEFARMER);
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
                if(sling < TARGET_SLINGER && g_frame < WAVE1_FRAME + SEC(30)
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
           && g_frame - g_priestLastMoveFrame >= 200)
        {
            orderMove(info, g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_frame;
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
                  || (g_frame >= FRAME_PRIEST_RETURN &&
                      (!g_wave1Handled || g_frame >= SEC(480)));
    if(goHome)
    {
        if(pd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH
           && g_frame - g_priestLastMoveFrame >= 300)
        {
            orderMove(info, g_priestSN, g_priestPointDR, g_priestPointUR);
            g_priestLastMoveFrame = g_frame;
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
    if(g_frame - g_priestLastMoveFrame >= 400)
    {
        orderMove(info, g_priestSN, tx, ty);
        g_priestLastMoveFrame = g_frame;
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
        // 观察期且敌兵进入20格，或到了4:40保险时间，进入防守
        if((g_frame >= FRAME_WATCH && nd <= 20.0) || g_frame >= FRAME_HARD_DEFENSE)
        {
            g_defenseMode = true;
            g_calmFrames = 0;
            dbg(QString("[防守] 第一波接近(最近敌兵%1格)，农民撤回TC!")
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

    // 防守期：所有陆地农民撤到TC旁抱团点（250帧刷新一次走位）
    if(g_defenseMode)
    {
        for(const auto& f : info.farmers)
        {
            if(f.FarmerSort != FARMERTYPE_FARMER) continue;
            double dd = d2(f.DR, f.UR, g_rallyDR, g_rallyUR);
            if(dd > 3.0*3.0*BLOCKSIDELENGTH*BLOCKSIDELENGTH)
            {
                static unordered_map<int,int> lastRetreat;
                auto it = lastRetreat.find(f.SN);
                int last = (it==lastRetreat.end()) ? -99999 : it->second;
                if(g_frame - last >= 250)
                {
                    orderMove(info, f.SN, g_rallyDR, g_rallyUR);
                    lastRetreat[f.SN] = g_frame;
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
                if(g_frame - last >= 500)
                {
                    orderMove(info, a.SN, g_priestPointDR, g_priestPointUR);
                    lastRally[a.SN] = g_frame;
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
            t = usrNearestResource(info, RESOURCE_TREE, f->DR, f->UR, false);
        else if(role == ROLE_BUSH)
        {
            t = usrNearestResource(info, RESOURCE_BUSH, f->DR, f->UR, false);
            if(t == -1) // 浆果采光：种空闲田，再不行采瞪羚
            {
                t = findFreeFarm(info);
                if(t == -1) t = usrNearestResource(info, RESOURCE_GAZELLE, f->DR, f->UR, false);
            }
        }
        else if(role == ROLE_FARM)
        {
            t = findFreeFarm(info);
            if(t == -1) t = usrNearestResource(info, RESOURCE_BUSH, f->DR, f->UR, false);
        }
        else if(role == ROLE_GOLD)
            t = usrNearestResource(info, RESOURCE_GOLD, f->DR, f->UR, false);
        else if(role == ROLE_STONE)
            t = usrNearestResource(info, RESOURCE_STONE, f->DR, f->UR, false);
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
    g_frame = info.GameFrame;
    g_orderedThisFrame.clear();

    // 低频状态日志
    if(g_frame % 300 == 0)
    {
        dbg(QString("[%1帧 %2:%3] 人口%4/%5 食%6 木%7 石%8 金%9 时代%10 防%11")
                  .arg(g_frame).arg(g_frame/FPS/60,2,10,QChar('0')).arg(g_frame/FPS%60,2,10,QChar('0'))
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
