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

// ---------- 村民岗位枚举（role map 的取值） ----------
enum UsrRole {
    ROLE_NONE   = 0,  // 未分配
    ROLE_BUILDER= 1,  // 专职建筑工（全程只负责建造链）
    ROLE_WOOD   = 2,  // 伐木
    ROLE_BUSH   = 3,  // 采浆果
    ROLE_HUNTER = 4,  // 双人猎瞪羚
    ROLE_FARM   = 5,  // 种田
    ROLE_GOLD   = 6,  // 采金（铜器后）
    ROLE_STONE  = 7,  // 采石（备用）
    ROLE_TMPB   = 8   // 临时建筑工（建农田，建完转 ROLE_FARM）
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
extern int  g_newFarmerIdx;                  // 新出生村民序号（每帧最多分配1个）
extern bool g_initDone;                      // 开局8人初始分配是否完成

// 防守阵地（箭塔/TC）
extern int  g_defTowerSN;                    // 主防守箭塔SN（-1=暂无，用TC）
extern int  g_defTowerDR, g_defTowerUR;      // 主防守箭塔块坐标
extern int  g_tcDR, g_tcUR;                  // TC块坐标
extern bool g_needNewTower;                  // 预置塔离TC太远，需要补建第二塔
extern bool g_towerTechIssued;               // 箭塔科技已下单
extern bool g_towerTechBusy;                 // 见过科技研发中
extern bool g_towerTechDone;                 // 箭塔科技已完成

// 防守状态机
extern bool g_defenseMode;                   // 第一波防守模式（农民抱团）
extern int  g_calmFrames;                    // 身边持续无敌兵的帧数
extern bool g_wave1Handled;                  // 第一波已撑过（用于日志/状态切换）

// 祭司
extern int  g_priestSN;
extern int  g_priestScoutIdx;                // 8方向探路下标
extern int  g_priestLastMoveFrame;           // 移动指令节流（400帧）
extern int  g_priestConvertTarget;           // 当前正在转换的目标SN
extern double g_priestPointDR, g_priestPointUR; // 塔后安全点（细节坐标）
extern double g_rallyDR, g_rallyUR;          // 农民抱团点（TC旁，细节坐标）

// 建造去重/节流
extern std::unordered_map<int,int> g_lastBuildTry;   // 建筑类型 -> 上次尝试帧
extern std::set<int> g_tmpBuilders;                  // 临时建农田的村民
extern std::unordered_map<int,int> g_tmpRetry;       // 临时建筑工重试节流
extern bool g_huntStockBuilt;                        // 猎场旁仓库已建
extern bool g_goldStockBuilt;                        // 金矿旁仓库已建

// 科技去重
extern bool g_woodTechIssued;
extern bool g_compositeIssued;

// 每帧"同一SN只下一条命令"去重
extern std::set<int> g_orderedThisFrame;

#endif // USRAI_H
