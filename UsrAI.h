#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <unordered_set>

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
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id=UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/
};

/*##########YOUR CODE BEGINS HERE##########*/

#include <vector>

// 村民工作类型枚举：记录每个村民被 AI 指派的职责。
// 按开局经济建议分配：4 人浆果、2 人木头、1 人石头、1 人建设者预留。
enum FarmerJob
{
    FARMER_JOB_UNASSIGNED = 0, // 未分配：尚未被 AI 指派任何工作
    FARMER_JOB_BERRY,          // 采集浆果（食物）
    FARMER_JOB_WOOD,           // 采集木材
    FARMER_JOB_STONE,          // 采集石头
    FARMER_JOB_BUILDER         // 建设者预留：不派采集任务，等待后续建房轮次
};

// 保存 AI 在不同游戏帧之间需要记住的信息。
// 这里目前只记录第一轮需要的关键对象和基地朝向，不包含任何行动策略。
struct AiRuntimeState
{
    int lastFrame = -1;              // 上一次处理的游戏帧，用于判断是否开始了新对局
    int lastReportFrame = -1;        // 上一次输出状态信息时的游戏帧
    bool initialized = false;        // 市镇中心、仓库、谷仓和祭司是否已经全部找到
    bool initializationReported = false; // 是否已经输出过本局的初始化信息

    int centerSn = -1;               // 市镇中心的唯一编号 SN
    int priestSn = -1;               // 祭司的唯一编号 SN
    int stockSn = -1;                // 仓库的唯一编号 SN
    int granarySn = -1;              // 谷仓的唯一编号 SN
    std::vector<int> arrowTowerSns;   // 我方所有已有箭塔的 SN

    int baseDR = -1;                 // 基地（市镇中心）的 DR 区块坐标
    int baseUR = -1;                 // 基地（市镇中心）的 UR 区块坐标
    int towardCenterDR = 0;          // DR 方向上朝地图中心移动的符号：-1、0 或 1
    int towardCenterUR = 0;          // UR 方向上朝地图中心移动的符号：-1、0 或 1

    bool initialFarmersCaptured = false; // 是否已经记录本局开局时存在的村民
    std::vector<int> initialFarmerSns;   // 开局村民的 SN，后续新生产的村民不属于本轮范围
    std::vector<int> nearbyBushSns;      // 基地附近仍可采集的浆果丛
    std::vector<int> nearbyTreeSns;      // 基地附近仍可采集的树木
    std::vector<int> nearbyStoneSns;     // 基地附近仍可采集的石矿
    std::vector<int> nearbyGoldSns;     // 基地附近仍可采集的金矿
    std::unordered_map<int, int> farmerGatherTargets; // 村民 SN -> 已分配资源 SN
    std::unordered_map<int, int> pendingGatherOrders; // 采集指令 ID -> 村民 SN
    std::unordered_map<int, int> farmerJobs;         // 村民 SN -> FarmerJob 工作类型
    std::unordered_map<int, int> farmerOrderFrame;   // 村民 SN -> 最近一次下令的帧号，用于下令后冷却

    // 市镇中心持续生产村民相关状态
    int pendingCenterProductionOrder = -1; // 正在等待 ins_ret 结果的造村民指令 ID，-1 表示无待确认指令
    int lastCenterProductionFrame = -1;    // 上次发起造村民指令的帧号，失败后用于间隔重试

    // 建设者与建房相关状态
    int builderSn = -1;                        // 当前固定建设者 SN，-1 表示尚未指定
    int pendingBuildOrder = -1;               // 待确认的建房指令 ID，-1 表示无
    int buildSiteSn = -1;                      // 正在建造中的房屋建筑 SN，-1 表示无在建房屋
    int lastBuildFrame = -1;                   // 上次发起建房指令的帧号
    int lastBuildBlockDR = -1;                // 上次建房指令的块坐标 DR（失败时加入黑名单）
    int lastBuildBlockUR = -1;                // 上次建房指令的块坐标 UR
    std::unordered_set<int> failedBuildPositions; // 近期失败的候选位置（编码为 BlockDR*1000+BlockUR），避免重复试同一位置
    bool builderNeedsWood = false;             // 房屋建成后，固定建设者是否需要被派去临时伐木

    // 祭司分阶段探路状态机
    int priestState = 0;                // 0=探索出发 1=返程 2=箭塔旁防守等第一波 3=第一波后再探索
    int pendingPriestOrder = -1;       // 待确认的祭司移动指令 ID
    int priestExploreStep = 0;         // 已向中心推进的段数（每段 5 格）
    int priestMaxBlood = -1;            // 记录祭司最大血量，用于检测掉血
    bool waveSeen = false;              // 是否已经看到过第一波敌军
    int noEnemyFrames = 0;              // 基地附近连续无敌军的帧数
    int lastPriestMoveFrame = -1;       // 上次给祭司下移动指令的帧
    std::unordered_set<int> visitedSites; // 已访问过的侦察点（编码为 blockDR*100+blockUR）
    double lastDistanceToTarget = -1.0; // 上次检查时祭司与目标的距离（用于卡住检测）
    int lastDistanceCheckFrame = -1;    // 上次距离检查帧
    int lateralSide = 0;                // 横向探索方向：-1=左，1=右，0=未开始
    bool atMaxRange = false;            // 是否已到达25格最大半径
    bool productionStoppedLogged = false; // 已输出过停止生产日志
};

// 全局状态对象定义在 UsrAI.cpp 中，供后续各轮策略继续使用。
extern AiRuntimeState gAiState;

/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H
