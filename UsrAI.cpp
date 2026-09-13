#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

/* ============================================================
 * 阶段0 开局：8村民 4采果+3伐木+1builder修房，祭司探路
 * 阶段1 打猎：新村民1去猎物旁等待，新村民2汇合后双人合猎；
 *              打完修仓库，之后新村民全部采集羚羊(6人)
 * 阶段2 市场+箭塔：谷仓→箭塔科技→箭塔；市场→伐木科技；
 *               第一波敌人靠箭塔+祭司转换挡下
 * 阶段3 兵营靶场+升级：兵营→靶场；食物800升级(工具/铜器)；
 *                升级后靶场爆复合弓兵，金矿仓库，村民采金
 * 阶段4 马厩学院+经济：马厩(骑兵)、学院(方阵兵)、10片农田；
 *                  第二波靠祭司+复合弓兵硬扛
 * 阶段5 三线爆兵：靶场复合弓兵+马厩骑兵+学院方阵兵同时造，
 *                  口满50；第三波集火全歼后即可推图
 * ============================================================ */

// ================== 全局状态变量定义 ==================
int m_gameStage = 0;

int worker_fruit[6] = {-1,-1,-1,-1,-1,-1};
int worker_wood[3] = {-1,-1,-1};
int worker_builder = -1;

int new_farmer_idx = 0;

int hunter_wait_sn = -1;
int hunter_partner_sn = -1;
bool hunter_waiting = false;
bool build_storage_after_hunt = false;

int priestSN = -1;
double priest_safeDR = 0;
double priest_safeUR = 0;
bool m_priest_moving = false;
int priest_explore_idx = 0;
int priest_last_move_frame = -1000;

set<int> assigned_farmer_sn;
set<int> gazelle_worker_sn;
set<int> gold_worker_sn;
set<int> stone_worker_sn;

bool m_research_arrowtower = false;
bool m_research_wood = false;
bool m_research_gold = false;
bool m_research_composite = false;

int g_buildTry[BUILDING_TYPE_MAXNUM] = {0};

// ================== 内部辅助函数 ==================

// 查找村民对象
const tagFarmer* getFarmer(const tagInfo& info, int sn)
{
    for(const auto& f : info.farmers)
    {
        if(f.SN == sn) return &f;
    }
    return nullptr;
}

// 该类型建筑是否已存在（含正在建造 Percent>0）
bool hasBuilding(const tagInfo& info, int type)
{
    for(const auto& b : info.buildings)
    {
        if(b.Type == type && b.Percent > 0) return true;
    }
    return false;
}

// 已建成的该类型建筑数量
int countBuilding(const tagInfo& info, int type)
{
    int cnt = 0;
    for(const auto& b : info.buildings)
    {
        if(b.Type == type && b.Percent >= 100) cnt++;
    }
    return cnt;
}

// =====辅助：从市镇中心附近寻找可建造的空地（带偏移，避免死磕同一位置）=====
bool findFlatBlock(int &outDR, int &outUR, const tagInfo& info)
{
    if(info.theMap == nullptr) return false;
    int rows = (int)info.theMap->size();
    if(rows == 0) return false;
    int cols = (int)(*info.theMap)[0].size();
    if(cols == 0) return false;

    int cDR = rows/2, cUR = cols/2;
    for(const auto& b : info.buildings)
    {
        if(b.Type == BUILDING_CENTER)
        {
            cDR = b.BlockDR;
            cUR = b.BlockUR;
            break;
        }
    }

    // 8个方向：北、东北、东、东南、南、西南、西、西北
    static const int dirX[8] = {1,1,0,-1,-1,-1,0,1};
    static const int dirY[8] = {0,1,1,1,0,-1,-1,-1};

    // 每次调用从不同方向开始搜索，避免连续建造死磕同一位置
    static int s_rot = 0;
    int startDir = s_rot;
    s_rot = (s_rot + 1) % 8;

    for(int r = 3; r <= 12; r++)
    {
        for(int k = 0; k < 8; k++)
        {
            int dir = (startDir + k) % 8;
            int dr = cDR + dirX[dir] * r;
            int ur = cUR + dirY[dir] * r;
            if(dr < 2 || ur < 2 || dr >= rows-2 || ur >= cols-2) continue;

            // 3x3 地块必须都是草地
            bool ok = true;
            for(int i=-1;i<=1 && ok;i++)
                for(int j=-1;j<=1 && ok;j++)
                {
                    if((*info.theMap)[dr+i][ur+j].type != MAPPATTERN_GRASS) ok = false;
                }
            if(!ok) continue;

            // 3x3 高度一致
            int h = (*info.theMap)[dr][ur].height;
            bool sameH = true;
            for(int i=-1;i<=1 && sameH;i++)
                for(int j=-1;j<=1 && sameH;j++)
                    if((*info.theMap)[dr+i][ur+j].height != h) sameH = false;
            if(!sameH) continue;

            // 不与已有建筑重叠（含在建）
            bool occupied = false;
            for(const auto& b : info.buildings)
            {
                int dx = abs(b.BlockDR - dr), dy = abs(b.BlockUR - ur);
                if(dx <= 1 && dy <= 1) { occupied = true; break; }
            }
            if(occupied) continue;

            // 不与资源（树/石头/浆果）重叠
            for(const auto& r : info.resources)
            {
                if(r.BlockDR < 0 || r.BlockUR < 0) continue;
                int dx = abs(r.BlockDR - dr), dy = abs(r.BlockUR - ur);
                if(dx <= 1 && dy <= 1) { occupied = true; break; }
            }
            if(occupied) continue;

            outDR = dr;
            outUR = ur;
            return true;
        }
    }
    return false;
}

// =====辅助：找最近资源SN=====
int findNearestResource(const tagInfo& info, int resType, int farmerSN)
{
    const tagFarmer* pFarmer = getFarmer(info, farmerSN);
    if(pFarmer == nullptr) return -1;

    double minDist = 1e12;
    int bestSN = -1;
    for(const auto& res : info.resources)
    {
        if(res.Type != resType) continue;
        double dx = pFarmer->DR - res.DR;
        double dy = pFarmer->UR - res.UR;
        double dist = dx*dx + dy*dy;
        if(dist < minDist)
        {
            minDist = dist;
            bestSN = res.SN;
        }
    }
    return bestSN;
}

// =====辅助：获取祭司SN（祭司在 armies 里！）=====
int getPriestSN(const tagInfo& info)
{
    for(const auto& army : info.armies)
    {
        if(army.Sort == AT_PRIEST)
        {
            return army.SN;
        }
    }
    return -1;
}

// 找一座尚未采完的农田SN（供空闲村民种田）
int findFarmToWork(const tagInfo& info)
{
    for(const auto& b : info.buildings)
    {
        if(b.Type == BUILDING_FARM && b.Percent >= 100 && b.Cnt > 0)
        {
            return b.SN;
        }
    }
    return -1;
}

// ================== 主入口 ==================
void UsrAI::processData ()
{
    tagInfo info = getInfo();
    if(info.theMap == nullptr)
    {
        return;
    }

    //======== 0、低频状态日志 ========
    if(info.GameFrame % 600 == 0)
    {
        DebugText(QString("[%1帧] 人口:%2/%3 食物:%4 木:%5 金:%6 石:%7 时代:%8")
                  .arg(info.GameFrame)
                  .arg(info.Human_Num).arg(info.Human_MaxNum)
                  .arg(info.Meat).arg(info.Wood).arg(info.Gold).arg(info.Stone)
                  .arg(info.civilizationStage));
    }

    //======== 1、市镇中心：持续造村民，直到人口满 ========
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_CENTER) continue;
        if(b.Percent < 100) continue;
        if(b.Project != ACT_NULL) continue;
        if(info.Human_Num >= info.Human_MaxNum) continue;
        if(info.Meat < BUILDING_CENTER_CREATEFARMER_FOOD) continue;
        BuildingAction(b.SN, BUILDING_CENTER_CREATEFARMER);
    }

    //======== 2、时代升级：石器→工具(500食物) → 铜器(800食物) ========
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_CENTER || b.Percent < 100) continue;
        if(b.Project != ACT_NULL) continue;
        if(info.civilizationStage == CIVILIZATION_STONEAGE && info.Meat >= 500)
        {
            BuildingAction(b.SN, BUILDING_CENTER_UPGRADE);
            DebugText(QString("升级到工具时代"));
        }
        else if(info.civilizationStage == CIVILIZATION_TOOLAGE && info.Meat >= 800)
        {
            BuildingAction(b.SN, BUILDING_CENTER_UPGRADE);
            DebugText(QString("升级到铜器时代"));
        }
    }

    //======== 3、祭司逻辑：探路→回防+转换 ========
    priestSN = getPriestSN(info);
    if(priestSN != -1)
    {
        // 祭司对象在 armies 里
        const tagArmy* priest = nullptr;
        for(const auto& a : info.armies)
        {
            if(a.SN == priestSN) { priest = &a; break; }
        }

        if(priest != nullptr)
        {
            // 安全点 = 市镇中心附近
            double safeDR = 30 * BLOCKSIDELENGTH;
            double safeUR = 30 * BLOCKSIDELENGTH;
            for(const auto& b : info.buildings)
            {
                if(b.Type == BUILDING_CENTER)
                {
                    safeDR = b.BlockDR * BLOCKSIDELENGTH;
                    safeUR = b.BlockUR * BLOCKSIDELENGTH;
                    break;
                }
            }
            priest_safeDR = safeDR;
            priest_safeUR = safeUR;

            // 找最近敌人（军队+农民）
            int enemySN = -1;
            double bestD = 1e18;
            for(const auto& e : info.enemy_armies)
            {
                double dx = priest->DR - e.DR;
                double dy = priest->UR - e.UR;
                double d = dx*dx + dy*dy;
                if(d < bestD) { bestD = d; enemySN = e.SN; }
            }
            for(const auto& e : info.enemy_farmers)
            {
                double dx = priest->DR - e.DR;
                double dy = priest->UR - e.UR;
                double d = dx*dx + dy*dy;
                if(d < bestD) { bestD = d; enemySN = e.SN; }
            }

            // 敌人进入约15格范围且祭司可转换 → 转换
            if(enemySN != -1 && bestD < 15*BLOCKSIDELENGTH*15*BLOCKSIDELENGTH
               && priest->ConvertCooldown == 0)
            {
                HumanAction(priestSN, enemySN);
                DebugText(QString("祭司转换敌人 %1").arg(enemySN));
            }
            // 没有可转换目标 → 移动（帧间隔节流，防止每帧重复发包）
            else if(info.GameFrame - priest_last_move_frame >= 400)
            {
                if(info.GameFrame < 6000)
                {
                    // 前期探路：8方向依次走出去
                    static const int dx8[8] = {2,2,0,-2,-2,-2,0,2};
                    static const int dy8[8] = {0,2,2,2,0,-2,-2,-2};
                    int idx = priest_explore_idx % 8;
                    priest_explore_idx++;
                    double tx = safeDR + dx8[idx]*10*BLOCKSIDELENGTH;
                    double ty = safeUR + dy8[idx]*10*BLOCKSIDELENGTH;
                    HumanMove(priestSN, tx, ty);
                }
                else
                {
                    // 6000帧后回基地守家（躲在市镇中心附近）
                    HumanMove(priestSN, safeDR + 3*BLOCKSIDELENGTH, safeUR + 3*BLOCKSIDELENGTH);
                }
                priest_last_move_frame = info.GameFrame;
            }
        }
    }

    //======== 4、开局8村民分配：4采果 + 3伐木 + 1builder ========
    if(info.farmers.size() >= 8)
    {
        int cnt_fruit = 0, cnt_wood = 0;
        for(int i=0;i<6;i++) if(worker_fruit[i] != -1) cnt_fruit++;
        for(int i=0;i<3;i++) if(worker_wood[i] != -1) cnt_wood++;

        if(cnt_fruit < 4 || cnt_wood < 3 || worker_builder == -1)
        {
            for(const auto& farmer : info.farmers)
            {
                int sn = farmer.SN;
                if(assigned_farmer_sn.count(sn)) continue;

                if(cnt_fruit < 4)
                {
                    worker_fruit[cnt_fruit] = sn;
                    assigned_farmer_sn.insert(sn);
                    int resSn = findNearestResource(info, RESOURCE_BUSH, sn);
                    if(resSn != -1) HumanAction(sn, resSn);
                    cnt_fruit++;
                }
                else if(cnt_wood < 3)
                {
                    worker_wood[cnt_wood] = sn;
                    assigned_farmer_sn.insert(sn);
                    int resSn = findNearestResource(info, RESOURCE_TREE, sn);
                    if(resSn != -1) HumanAction(sn, resSn);
                    cnt_wood++;
                }
                else if(worker_builder == -1)
                {
                    worker_builder = sn;
                    assigned_farmer_sn.insert(sn);
                }
            }
        }
    }

    //======== 5、新出生村民分配：每帧最多分配1个，防止new_farmer_idx暴涨 ========
    {
        int newSn = -1;
        for(const auto& farmer : info.farmers)
        {
            if(!assigned_farmer_sn.count(farmer.SN))
            {
                newSn = farmer.SN;
                break;
            }
        }

        if(newSn != -1)
        {
            assigned_farmer_sn.insert(newSn);

            if(new_farmer_idx == 0 || new_farmer_idx == 1)
            {
                // 第0、1号新村民：继续采果子（凑够6人采果）
                worker_fruit[4 + new_farmer_idx] = newSn;
                int resSn = findNearestResource(info, RESOURCE_BUSH, newSn);
                if(resSn != -1) HumanAction(newSn, resSn);
            }
            else if(new_farmer_idx == 2)
            {
                // 第2个：猎人1，站到猎物旁等待
                DebugText(QString("猎人一号，去猎物旁等待队友"));
                hunter_wait_sn = newSn;
                hunter_waiting = true;
                int prey = findNearestResource(info, RESOURCE_GAZELLE, newSn);
                if(prey != -1)
                {
                    for(const auto& res : info.resources)
                    {
                        if(res.SN == prey)
                        {
                            HumanMove(newSn, res.DR, res.UR);
                            break;
                        }
                    }
                }
            }
            else if(new_farmer_idx == 3)
            {
                // 第3个：猎人二号，去汇合猎人1
                DebugText(QString("猎人二号，去汇合"));
                hunter_partner_sn = newSn;
                const tagFarmer* f1 = getFarmer(info, hunter_wait_sn);
                if(f1 != nullptr)
                {
                    HumanMove(newSn, f1->DR, f1->UR);
                }
            }
            else if(new_farmer_idx >= 4 && new_farmer_idx <= 9)
            {
                // 第4~9个：采集羚羊（6人）
                DebugText(QString("新村民分配采集羚羊"));
                gazelle_worker_sn.insert(newSn);
                int resSn = findNearestResource(info, RESOURCE_GAZELLE, newSn);
                if(resSn != -1) HumanAction(newSn, resSn);
            }
            else
            {
                // 第10个之后：箭塔未建且石头不足 → 采石；否则采金
                bool needStone = !hasBuilding(info, BUILDING_ARROWTOWER)
                                 && info.Stone < BUILD_ARROWTOWER_STONE;
                if(needStone)
                {
                    DebugText(QString("新村民分配采石"));
                    stone_worker_sn.insert(newSn);
                    int resSn = findNearestResource(info, RESOURCE_STONE, newSn);
                    if(resSn != -1) HumanAction(newSn, resSn);
                }
                else
                {
                    DebugText(QString("新村民分配采金"));
                    gold_worker_sn.insert(newSn);
                    int resSn = findNearestResource(info, RESOURCE_GOLD, newSn);
                    if(resSn != -1) HumanAction(newSn, resSn);
                }
            }
            new_farmer_idx++;
        }
    }

    //======== 6、空闲村民自动续任务（防止挂机） ========
    // 采果村民：空闲→继续采果；果子采完→种田或采金
    for(int i=0;i<6;i++)
    {
        int sn = worker_fruit[i];
        if(sn == -1) continue;
        const tagFarmer* fptr = getFarmer(info, sn);
        if(fptr == nullptr || fptr->NowState != HUMAN_STATE_IDLE) continue;

        int resSn = findNearestResource(info, RESOURCE_BUSH, sn);
        if(resSn != -1) HumanAction(sn, resSn);
        else
        {
            // 浆果采完：去种田，田也没有就采金
            int farmSN = findFarmToWork(info);
            if(farmSN != -1) HumanAction(sn, farmSN);
            else
            {
                int g = findNearestResource(info, RESOURCE_GOLD, sn);
                if(g != -1) { HumanAction(sn, g); gold_worker_sn.insert(sn); }
            }
        }
    }

    // 伐木村民：空闲→继续砍树
    for(int i=0;i<3;i++)
    {
        int sn = worker_wood[i];
        if(sn == -1) continue;
        const tagFarmer* fptr = getFarmer(info, sn);
        if(fptr == nullptr || fptr->NowState != HUMAN_STATE_IDLE) continue;
        int resSn = findNearestResource(info, RESOURCE_TREE, sn);
        if(resSn != -1) HumanAction(sn, resSn);
    }

    // 羚羊采集村民：空闲→继续采羚羊；没有→种田或采金
    for(auto it = gazelle_worker_sn.begin(); it != gazelle_worker_sn.end(); )
    {
        int sn = *it;
        const tagFarmer* fptr = getFarmer(info, sn);
        if(fptr == nullptr) { it = gazelle_worker_sn.erase(it); continue; }
        if(fptr->NowState == HUMAN_STATE_IDLE)
        {
            int resSn = findNearestResource(info, RESOURCE_GAZELLE, sn);
            if(resSn != -1) HumanAction(sn, resSn);
            else
            {
                int farmSN = findFarmToWork(info);
                if(farmSN != -1) HumanAction(sn, farmSN);
                else
                {
                    int g = findNearestResource(info, RESOURCE_GOLD, sn);
                    if(g != -1) { HumanAction(sn, g); gold_worker_sn.insert(sn); }
                }
            }
        }
        ++it;
    }

    // 采金村民：空闲→继续采金
    for(auto it = gold_worker_sn.begin(); it != gold_worker_sn.end(); )
    {
        int sn = *it;
        const tagFarmer* fptr = getFarmer(info, sn);
        if(fptr == nullptr) { it = gold_worker_sn.erase(it); continue; }
        if(fptr->NowState == HUMAN_STATE_IDLE)
        {
            int resSn = findNearestResource(info, RESOURCE_GOLD, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
        ++it;
    }

    // 采石村民：空闲→继续采石
    for(auto it = stone_worker_sn.begin(); it != stone_worker_sn.end(); )
    {
        int sn = *it;
        const tagFarmer* fptr = getFarmer(info, sn);
        if(fptr == nullptr) { it = stone_worker_sn.erase(it); continue; }
        if(fptr->NowState == HUMAN_STATE_IDLE)
        {
            int resSn = findNearestResource(info, RESOURCE_STONE, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
        ++it;
    }

    //======== 7、双人打猎逻辑 ========
    if(hunter_wait_sn != -1 && hunter_partner_sn != -1)
    {
        // 找一只羚羊
        int preySN = -1;
        for(const auto& res : info.resources)
        {
            if(res.Type == RESOURCE_GAZELLE)
            {
                preySN = res.SN;
                break;
            }
        }

        if(preySN != -1)
        {
            // 两人空闲时各自打猎（避免每帧重复发包）
            const tagFarmer* f1 = getFarmer(info, hunter_wait_sn);
            const tagFarmer* f2 = getFarmer(info, hunter_partner_sn);
            if(f1 != nullptr && f1->NowState == HUMAN_STATE_IDLE)
                HumanAction(hunter_wait_sn, preySN);
            if(f2 != nullptr && f2->NowState == HUMAN_STATE_IDLE)
                HumanAction(hunter_partner_sn, preySN);
        }
        else
        {
            // 猎物打完：猎人转去建仓库，之后转采金/种田
            DebugText(QString("猎物全部打完，猎人开始建仓库"));
            hunter_waiting = false;
            build_storage_after_hunt = true;
            hunter_wait_sn = -1;
            hunter_partner_sn = -1;
        }
    }

    //======== 8、打猎结束后：建仓库 ========
    if(build_storage_after_hunt)
    {
        if(!hasBuilding(info, BUILDING_STOCK))
        {
            // 找一个空闲村民建仓库
            for(const auto& f : info.farmers)
            {
                if(f.NowState != HUMAN_STATE_IDLE) continue;
                if(f.SN == worker_builder) continue;
                if(info.Wood < BUILD_STOCK_WOOD) break;
                int x, y;
                if(findFlatBlock(x, y, info))
                {
                    HumanBuild(f.SN, BUILDING_STOCK, x, y);
                    DebugText(QString("猎人建造仓库"));
                }
                break;
            }
        }
        else
        {
            build_storage_after_hunt = false;
        }
    }

    //======== 9、专职builder：按优先级建造 ========
    if(worker_builder != -1)
    {
        const tagFarmer* bf = getFarmer(info, worker_builder);
        if(bf != nullptr && bf->NowState == HUMAN_STATE_IDLE)
        {
            int houseCnt   = countBuilding(info, BUILDING_HOME);
            bool hasGranary= hasBuilding(info, BUILDING_GRANARY);
            bool hasCamp   = hasBuilding(info, BUILDING_ARMYCAMP);
            bool hasMarket = hasBuilding(info, BUILDING_MARKET);
            bool hasTower  = hasBuilding(info, BUILDING_ARROWTOWER);
            bool hasRange  = hasBuilding(info, BUILDING_RANGE);
            bool hasStable = hasBuilding(info, BUILDING_STABLE);
            bool hasCollage= hasBuilding(info, BUILDING_COLLAGE);
            int farmCnt    = countBuilding(info, BUILDING_FARM);
            int stage      = info.civilizationStage;

            int buildType = -1;
            // 建筑优先级
            if(houseCnt < 2)
            {
                buildType = BUILDING_HOME;                 // 开局先修2房
            }
            else if(!hasGranary)
            {
                buildType = BUILDING_GRANARY;              // 谷仓（箭塔科技载体）
            }
            else if(!hasCamp)
            {
                buildType = BUILDING_ARMYCAMP;             // 兵营（石器时代可建）
            }
            else if(stage >= CIVILIZATION_TOOLAGE && !hasMarket)
            {
                buildType = BUILDING_MARKET;               // 市场（工具时代）
            }
            else if(stage >= CIVILIZATION_TOOLAGE && !hasTower
                    && m_research_arrowtower && info.Stone >= BUILD_ARROWTOWER_STONE)
            {
                buildType = BUILDING_ARROWTOWER;           // 箭塔（需谷仓科技）
            }
            else if(stage >= CIVILIZATION_TOOLAGE && !hasRange)
            {
                buildType = BUILDING_RANGE;                // 靶场（工具时代）
            }
            else if(stage >= CIVILIZATION_TOOLAGE && !hasStable)
            {
                buildType = BUILDING_STABLE;               // 马厩（工具时代）
            }
            else if(stage >= CIVILIZATION_BRONZEAGE && !hasCollage)
            {
                buildType = BUILDING_COLLAGE;              // 学院（铜器时代）
            }
            else if(stage >= CIVILIZATION_TOOLAGE && hasMarket && farmCnt < 10)
            {
                buildType = BUILDING_FARM;                 // 10片农田
            }
            else if(houseCnt < 12)
            {
                buildType = BUILDING_HOME;                 // 房屋补到12个
            }

            if(buildType != -1)
            {
                int x, y;
                if(findFlatBlock(x, y, info))
                {
                    HumanBuild(worker_builder, buildType, x, y);
                    DebugText(QString("builder建造建筑类型%1 @(%2,%3)").arg(buildType).arg(x).arg(y));
                    g_buildTry[buildType]++;  // 失败时下一帧换方向
                }
            }
            else
            {
                // 建筑全部完成：builder 转采金
                int g = findNearestResource(info, RESOURCE_GOLD, worker_builder);
                if(g != -1) { HumanAction(worker_builder, g); gold_worker_sn.insert(worker_builder); }
            }
        }
    }

    //======== 10、科技研发 ========
    // 谷仓：研发建造箭塔（50食物）
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_GRANARY || b.Percent < 100) continue;
        if(b.Project != ACT_NULL) continue;
        if(!m_research_arrowtower && info.Meat >= 50)
        {
            BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
            m_research_arrowtower = true;
            DebugText(QString("谷仓研发箭塔科技"));
        }
    }

    // 市场：伐木科技(120食75木) → 金矿科技(120食100木)
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_MARKET || b.Percent < 100) continue;
        if(b.Project != ACT_NULL) continue;
        if(!m_research_wood && info.Meat >= 120 && info.Wood >= 75)
        {
            BuildingAction(b.SN, BUILDING_MARKET_WOOD_UPGRADE);
            m_research_wood = true;
            DebugText(QString("市场研发伐木科技"));
        }
        else if(m_research_wood && !m_research_gold && info.Meat >= 120 && info.Wood >= 100)
        {
            BuildingAction(b.SN, BUILDING_MARKET_GOLD_UPGRADE);
            m_research_gold = true;
            DebugText(QString("市场研发金矿科技"));
        }
    }

    // 靶场：复合弓科技（铜器时代，180食100木）
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_RANGE || b.Percent < 100) continue;
        if(b.Project != ACT_NULL) continue;
        if(!m_research_composite && info.civilizationStage >= CIVILIZATION_BRONZEAGE
           && info.Meat >= 180 && info.Wood >= 100)
        {
            BuildingAction(b.SN, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW);
            m_research_composite = true;
            DebugText(QString("靶场研发复合弓科技"));
        }
    }

    //======== 11、三线爆兵（铜器时代后） ========
    if(info.civilizationStage >= CIVILIZATION_BRONZEAGE)
    {
        for(const auto& b : info.buildings)
        {
            if(b.Percent < 100 || b.Project != ACT_NULL) continue;

            // 靶场：复合弓兵（40食20金）
            if(b.Type == BUILDING_RANGE && m_research_composite
               && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD
               && info.Gold >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD)
            {
                BuildingAction(b.SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
            }
            // 马厩：骑兵（70食80金）
            else if(b.Type == BUILDING_STABLE
                    && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD
                    && info.Gold >= BUILDING_STABLE_CREATE_CAVALRY_GOLD)
            {
                BuildingAction(b.SN, BUILDING_STABLE_CREATE_CAVALRY);
            }
            // 学院：方阵兵（60食40金）
            else if(b.Type == BUILDING_COLLAGE
                    && info.Meat >= BUILDING_COLLAGE_CREATE_HOPLITE_FOOD
                    && info.Gold >= BUILDING_COLLAGE_CREATE_HOPLITE_GOLD)
            {
                BuildingAction(b.SN, BUILDING_COLLAGE_CREATE_HOPLITE);
            }
        }
    }

    //======== 12、农田维护：农田采完(Cnt<=0)派空闲村民重新种 ========
    for(const auto& b : info.buildings)
    {
        if(b.Type != BUILDING_FARM || b.Percent < 100) continue;
        if(b.Cnt > 0) continue;
        for(const auto& f : info.farmers)
        {
            if(f.NowState != HUMAN_STATE_IDLE) continue;
            if(f.SN == worker_builder) continue;
            HumanAction(f.SN, b.SN);
            break;
        }
    }
}

