#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <set>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI:public AI
{
public:
    UsrAI(){this->id=0;}
    ~UsrAI(){}

private:
    void processData() override;
    int AddToIns(instruction ins) override
        {
            UsrIns.lock.lock();
            ins.id=UsrIns.g_id;
            UsrIns.g_id++;
            UsrIns.instructions.push(ins);
            UsrIns.lock.unlock();
            return ins.id;
        }
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/
};

/* =====================================================================
 * 以下是 AI 用到的全局状态与辅助函数声明
 * （本工程的 AI 状态按文件级全局变量保存，跨帧持久化）
 * ===================================================================== */

// ---------- 策略参数（帧数、资源成本和目标数量） ----------
constexpr int FPS = 25;
constexpr int SEC(int seconds) { return seconds * FPS; }

constexpr int FRAME_PRIEST_RETURN = SEC(200);
constexpr int FRAME_WATCH = SEC(220);
constexpr int FRAME_HARD_DEFENSE = SEC(280);

constexpr double PRIEST_BEHIND_TOWER_BLOCKS = 4.0;
constexpr double PRIEST_EMERGENCY_BEHIND_TOWER_BLOCKS = 2.0;
constexpr double TOWER_ATTACK_RANGE_BLOCKS = 7.0;
constexpr int TOWER_RESCUE_STABLE_FRAMES = 25;
constexpr double DEFENSE_TRIGGER_BLOCKS = 20.0;
constexpr double DEFENSE_SAFE_CORE_BLOCKS = 22.0;
constexpr double DEFENSE_SAFE_FARMER_BLOCKS = 10.0;
constexpr int DEFENSE_SAFE_FRAMES = 600;
constexpr int DEFENSE_CLEANUP_MAX_UNITS = 2;

constexpr int SIEGE_INTERCEPT_MAX_UNITS = 2;
constexpr double SIEGE_INTERCEPT_RADIUS_BLOCKS = 14.0;
constexpr int SIEGE_INTERCEPT_MIN_HEALTH_PERCENT = 35;

constexpr int FRAME_RANGED_SCOUT_RECALL = SEC(780);
constexpr double RANGED_SCOUT_RADIUS_BLOCKS = 25.0;
constexpr double RANGED_SCOUT_ENEMY_RANGE_BLOCKS = 15.0;
constexpr int RANGED_SCOUT_RETURN_HEALTH_PERCENT = 70;

constexpr int COST_FARMER_FOOD = 50;
constexpr int COST_BRONZE_FOOD = 800;
constexpr int COST_HOUSE_WOOD = 30;
constexpr int COST_STOCK_WOOD = 120;
constexpr int COST_CAMP_WOOD = 125;
constexpr int COST_MARKET_WOOD = 150;
constexpr int COST_RANGE_WOOD = 150;
constexpr int COST_FARM_WOOD = 75;
constexpr int COST_TOWERTECH_FOOD = 50;
constexpr int COST_COMPOSITE_FOOD = 40;
constexpr int COST_COMPOSITE_GOLD = 20;
constexpr int COST_TECH_WOOD_F = 120;
constexpr int COST_TECH_WOOD_W = 75;
constexpr int COST_TECH_COMP_F = 180;
constexpr int COST_TECH_COMP_W = 100;

constexpr int TARGET_BERRY_FARM_WORKERS = 6;
constexpr int MIN_LAND_FARMERS_FOR_HUNTER_PAIR = 12;
constexpr int POST_WAVE2_FARM_WOOD_RESERVE = 350;
constexpr int TARGET_GOLD_MINER = 2;
constexpr int TARGET_LAND_FARMERS = 20;
constexpr int PRE_BRONZE_LAND_FARMERS = 14;
constexpr int PRE_WAVE2_LAND_FARMERS = 16;
constexpr int TARGET_UPGRADE_FOOD_WORKERS = 11;
constexpr int MAX_HOUSES = 12;
constexpr double HUNT_SEARCH_RADIUS = 20.0;
constexpr double HUNT_CHASE_LIMIT = 25.0;
constexpr double HUNTER_MEET_DISTANCE = 2.0;

constexpr int KEY_STOCK_GOLD = 9002;

// ---------- 村民岗位枚举（role map 的取值） ----------
enum UsrRole {
    ROLE_NONE   = 0,  // 未分配
    ROLE_BUILDER= 1,  // 专职建筑工（全程只负责建造链）
    ROLE_WOOD   = 2,  // 伐木
    ROLE_BUSH   = 3,  // 采浆果
    ROLE_HUNTER = 4,  // 双人猎瞪羚
    ROLE_FARM   = 5,  // 种田
    ROLE_GOLD   = 6,  // 采金（铜器后）
    ROLE_STONE  = 7   // 采石（备用）
};

// ---------- 辅助函数（定义在 UsrAI.cpp） ----------
const tagFarmer*   usrGetFarmer(const tagInfo& info, int sn);
const tagArmy*     usrGetArmy(const tagInfo& info, int sn);
bool   usrHasBuilding(const tagInfo& info, int type);                 // 含在建(Percent>0)
int    usrCountBuilt(const tagInfo& info, int type);                  // 仅已建成(Percent>=100)
bool   usrFindFlatNear(int &outDR, int &outUR, const tagInfo& info,
                       int cDR, int cUR, int minR, int maxR);          // 以某块为中心环形找3x3平地
int    usrNearestResource(const tagInfo& info, int resType,
                          double fromDR, double fromUR, bool liveOnly);
int    usrBestGatherResource(const tagInfo& info, int resType,
                             const tagFarmer& farmer);
int    usrCountLandFarmers(const tagInfo& info);
int    usrNearestBuilding(const tagInfo& info, int type,
                          double fromDR, double fromUR);
double usrBlockDist(int aDR, int aUR, int bDR, int bUR);

// ---------- 全局状态变量（定义在 UsrAI.cpp） ----------
extern int  g_aiframe;                         // 缓存当前帧号

// 开局分工
extern std::unordered_map<int,int> g_role;   // 村民SN -> 岗位(UsrRole)
extern int  g_builderSN;                     // 专职建筑工SN
extern int  g_hunterSN[2];                   // 双人猎两个村民SN
extern bool g_hunterActive;                  // 双人猎是否启用
extern bool g_initDone;                      // 开局8人初始分配是否完成

// 防守阵地（箭塔/TC）
extern int  g_defTowerSN;                    // 主防守箭塔SN（-1=暂无，用TC）
extern int  g_defTowerDR, g_defTowerUR;      // 主防守箭塔块坐标
extern int  g_tcDR, g_tcUR;                  // TC块坐标
extern bool g_towerTechIssued;               // 箭塔科技已下单
extern bool g_towerTechBusy;                 // 见过科技研发中
extern bool g_towerTechDone;                 // 箭塔科技已完成

// 防守状态机
extern bool g_defenseMode;                   // 当前波次防守模式（箭塔主防，受攻击村民单独撤离）
extern int  g_calmFrames;                    // 持续无有效威胁的帧数
extern bool g_wave1Handled;                  // 第一波已撑过（用于日志/状态切换）
extern bool g_wave2Handled;                  // 第二波已撑过（村民阶段目标提升到20）
extern int  g_activeDefenseWave;             // 当前防守波次，进入防守时锁定

// 祭司
extern int  g_priestSN;
extern int  g_priestScoutIdx;                // 8方向探路下标
extern int  g_priestLastMoveFrame;           // 移动指令节流（400帧）
extern int  g_priestConvertTarget;           // 当前正在转换的目标SN
extern double g_priestPointDR, g_priestPointUR; // 塔后4格安全点（细节坐标）
extern double g_rallyDR, g_rallyUR;          // 防守阵地参考点（细节坐标）

// 建造去重/节流
extern std::unordered_map<int,int> g_lastBuildTry;   // 建筑类型 -> 上次尝试帧
extern bool g_goldStockBuilt;                        // 金矿旁仓库已建

// 科技去重
extern bool g_woodTechIssued;
extern bool g_woodTechBusy;
extern bool g_woodTechDone;
extern bool g_compositeIssued;
extern bool g_compositeTechBusy;
extern bool g_compositeTechDone;

// 每帧"同一SN只下一条命令"去重
extern std::set<int> g_orderedThisFrame;

#endif // USRAI_H
