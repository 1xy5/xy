#include "UsrAI.h"
#include<set>
#include <iostream>
#include<unordered_map>
#include<list>
#include <cstdlib>

using namespace std;
tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

void UsrAI::processData()
{
    // 第一步永远是获取当前帧状态（课设文档建议）
    info = getInfo();

    debugStatus();      // 每秒打印一次状态，验证帧循环在跑
    cleanPending();     // 先清理已生效的命令记录
    buildHouses();      // 人口快满时优先盖房（防止生产卡住）
    produceFarmers();   // 市镇中心持续生产村民
    assignIdleFarmers();// 空闲村民去采集
}

/* ==================== 调试输出 ====================
 * GameFrame 每 25 帧递增一次秒数（TimePerFrame=40ms，即 25帧/秒）。
 * 只在整秒时输出，避免 Debug 栏刷屏。
 */
void UsrAI::debugStatus()
{
    if (info.GameFrame % 25 != 0) return;
    DebugText(QString("[M1] t=%1s food=%2 wood=%3 stone=%4 gold=%5 pop=%6/%7 farmers=%8")
        .arg(info.GameFrame / 25)
        .arg(info.Meat).arg(info.Wood).arg(info.Stone).arg(info.Gold)
        .arg(info.Human_Num).arg(info.Human_MaxNum)
        .arg((int)info.farmers.size()));
}

/* ==================== 命令防重机制 ====================
 * 指令是异步的：下令后村民要等主线程处理才离开 IDLE 状态。
 * 若每帧都对同一个 IDLE 村民下令，会产生大量重复命令。
 * 解决：下令时把村民 SN 记入 pendingFarmers，观察到它离开 IDLE
 *（命令已生效）或从列表消失（已死亡）后再移除。
 */
void UsrAI::cleanPending()
{
    for (size_t i = 0; i < pendingFarmers.size(); ) {
        bool stillIdle = false;
        for (const tagFarmer& f : info.farmers) {
            if (f.SN == pendingFarmers[i]) {          // 村民还活着
                stillIdle = (f.NowState == HUMAN_STATE_IDLE);
                break;
            }
        }
        if (stillIdle) ++i;                           // 命令还在路上，继续保留
        else pendingFarmers.erase(pendingFarmers.begin() + i); // 已生效或已消失
    }
}

/* ==================== 空闲村民自动采集 ====================
 * 策略（简单版）：食物优先 -> 采最近的浆果丛；浆果采完 -> 伐木。
 * HumanAction(SN, 资源SN) 后村民会自动走过去采集并往返运送。
 * 改进方向：按资源缺口分配工种、浆果耗尽后转农田、加入打猎模块。
 */
void UsrAI::assignIdleFarmers()
{
    for (const tagFarmer& f : info.farmers) {
        if (f.NowState != HUMAN_STATE_IDLE) continue;
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;   // 只管陆地村民

        bool pending = false;
        for (int sn : pendingFarmers) if (sn == f.SN) { pending = true; break; }
        if (pending) continue;                             // 已有命令在路上

        int goalSN = findNearestResource(f, RESOURCE_BUSH); // 先保食物
        if (goalSN == -1)
            goalSN = findNearestResource(f, RESOURCE_TREE); // 浆果没了伐木
        if (goalSN == -1) continue;

        HumanAction(f.SN, goalSN);
        pendingFarmers.push_back(f.SN);
    }
}

/* ==================== 市镇中心持续生产村民 ====================
 * 条件：村民不足目标数、食物留有余量（50造价+100缓冲，攒升时代）、
 * 人口未满（留1个空位）。lastProduceFrame 做下令冷却，防止指令
 * 延迟期间每帧重复下令。
 * 改进方向：M1 之后把目标数提到 20 左右；触发升级时代逻辑。
 */
void UsrAI::produceFarmers()
{
    const int TARGET_FARMERS = 12;                     // M1 阶段目标
    if ((int)info.farmers.size() >= TARGET_FARMERS) return;
    if (info.Meat < BUILDING_CENTER_CREATEFARMER_FOOD + 100) return; // 留 100 食物缓冲
    if (info.Human_Num >= info.Human_MaxNum - 1) return;             // 留 1 人口空位
    if (info.GameFrame - lastProduceFrame < 25) return;              // 1秒冷却

    for (const tagBuilding& b : info.buildings) {
        if (b.Type == BUILDING_CENTER && b.Percent == 100 && b.Project == 0) {
            BuildingAction(b.SN, BUILDING_CENTER_CREATEFARMER);
            lastProduceFrame = info.GameFrame;
            break;
        }
    }
}

/* ==================== 自动盖房 ====================
 * 触发：人口余量 <= 4（一座房的容量）、木头够、且当前没有在建房屋。
 * 找一块 2x2 平地（避开建筑/资源），派一个空闲村民去盖。
 * 改进方向：谷仓附近规划房屋带；农田位预留；失败换地重试。
 */
void UsrAI::buildHouses()
{
    if (info.Human_MaxNum - info.Human_Num > HOUSE_HUMAN_NUM) return; // 余量还够
    if (info.Wood < BUILD_HOUSE_WOOD + 20) return;                    // 留点木头缓冲

    for (const tagBuilding& b : info.buildings)                       // 有在建房屋则等
        if (b.Type == BUILDING_HOME && b.Percent < 100) return;

    int x, y;
    if (!findSpot(2, x, y)) return;

    for (const tagFarmer& f : info.farmers) {                         // 派空闲村民去盖
        if (f.NowState != HUMAN_STATE_IDLE || f.FarmerSort != FARMERTYPE_FARMER) continue;
        bool pending = false;
        for (int sn : pendingFarmers) if (sn == f.SN) { pending = true; break; }
        if (pending) continue;

        HumanBuild(f.SN, BUILDING_HOME, x, y);
        pendingFarmers.push_back(f.SN);
        searchX = x + 2;  // 下次从这往后找，即使本次建造失败也不会卡死在同一块地
        searchY = y;
        return;
    }
}

/* ==================== 工具函数 ==================== */

// 块坐标曼哈顿距离下，离村民最近的 resType 资源SN；找不到返回 -1
int UsrAI::findNearestResource(const tagFarmer& f, int resType)
{
    int bestSN = -1, bestDist = 0x7fffffff;
    for (const tagResource& r : info.resources) {
        if (r.Type != resType || r.Cnt <= 0) continue;
        int d = abs(r.BlockDR - f.BlockDR) + abs(r.BlockUR - f.BlockUR);
        if (d < bestDist) { bestDist = d; bestSN = r.SN; }
    }
    return bestSN;
}

// (x,y) 为左下角的 size*size 区域是否为平坦草地且不与已知建筑/资源重叠
bool UsrAI::isBlockFree(int x, int y, int size)
{
    if (x < 1 || y < 1 || x + size > MAP_L || y + size > MAP_U) return false;

    tagTerrain t0 = (*info.theMap)[x][y];
    if (t0.type != MAPPATTERN_GRASS) return false;        // 海洋/未探索都排除
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j) {
            tagTerrain t = (*info.theMap)[x + i][y + j];
            if (t.type != MAPPATTERN_GRASS || t.height != t0.height) return false; // 有高度差不能建
        }

    for (const tagBuilding& b : info.buildings) {         // 建筑占地区域（含1格缓冲）
        int s = (b.Type == BUILDING_HOME || b.Type == BUILDING_ARROWTOWER) ? 2 : 3;
        if (x < b.BlockDR + s + 1 && x + size > b.BlockDR - 1 &&
            y < b.BlockUR + s + 1 && y + size > b.BlockUR - 1) return false;
    }
    for (const tagResource& r : info.resources) {         // 资源占 1 格
        if (x < r.BlockDR + 1 && x + size > r.BlockDR &&
            y < r.BlockUR + 1 && y + size > r.BlockUR) return false;
    }
    return true;
}

// 从上次搜索进度继续向后找空地（示例7思路：进度记忆，避免重复命中同一块地）
bool UsrAI::findSpot(int size, int& x, int& y)
{
    for (int i = searchX; i + size <= MAP_L - 1; ++i)
        for (int j = 1; j + size <= MAP_U - 1; ++j)
            if (isBlockFree(i, j, size)) { x = i; y = j; return true; }
    searchX = 1;                                          // 扫完了，从头再来
    return false;
}
