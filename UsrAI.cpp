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
/* ============================== 主入口 ============================== */
//=========【全局状态变量，对应UsrAI.h里面的extern声明】=========
int m_gameStage = 0;

bool m_built_storage = false;
bool m_built_granary = false;
bool m_built_barrack = false;
bool m_built_market = false;
bool m_built_range = false;

bool m_research_arrowtower = false;
int m_target_farmer_count = 24;

//=========辅助函数1：寻找平地建造建筑=========
bool findFlatBlock(int &outDR, int &outUR, const tagInfo& info)
{
    //地图为空，直接返回，禁止访问
    if(info.theMap == nullptr)
        return false;

    //上限改为58，dr最多57，dr+2=59，不会越界
    for(int dr = 15; dr < 58; dr++)
    {
        for(int ur =15; ur < 58; ur++)
        {
            tagTerrain t00 = (*info.theMap)[dr][ur];
            tagTerrain t10 = (*info.theMap)[dr+2][ur];
            tagTerrain t01 = (*info.theMap)[dr][ur+2];
            tagTerrain t11 = (*info.theMap)[dr+2][ur+2];

            if(t00.type != MAPPATTERN_GRASS) continue;
            if(t10.type != MAPPATTERN_GRASS) continue;
            if(t01.type != MAPPATTERN_GRASS) continue;
            if(t11.type != MAPPATTERN_GRASS) continue;

            int h = t00.height;
            if(t10.height == h && t01.height == h && t11.height == h)
            {
                outDR = dr;
                outUR = ur;
                return true;
            }
        }
    }
    return false;
}


//=========辅助函数2：寻找离村民最近的资源SN=========
int findNearestResource(const tagInfo& info, int resType, int farmerSN)
{
    const tagFarmer* pFarmer = nullptr;
    for(const auto &f : info.farmers)
    {
        if(f.SN == farmerSN)
        {
            pFarmer = &f;
            break;
        }
    }
    if(pFarmer == nullptr) return -1;

    double minDist = 1e12;
    int bestSN = -1;
    for(auto &res : info.resources)
    {
        if(res.Type != resType) continue;
        double dx = pFarmer->DR - res.DR;
        double dy = pFarmer->UR - res.UR;
        double dist = sqrt(dx*dx + dy*dy);
        if(dist < minDist)
        {
            minDist = dist;
            bestSN = res.SN;
        }
    }
    return bestSN;
}

//=========主入口函数 processData 每一帧自动调用=========
void UsrAI::processData ()
{
    tagInfo info = getInfo();
    // 防护代码
        if(info.theMap == nullptr)
        {
            return;
        }
    if(m_gameStage == 0)
    {
        DebugText(QString("【第一阶段 开局发育】frame=%1 木:%2 食:%3 石:%4 金:%5 村民数:%6")
                  .arg(info.GameFrame).arg(info.Wood).arg(info.Meat).arg(info.Stone).arg(info.Gold).arg(info.farmers.size()));

        //1. 市镇中心生产村民，目标24个
        for(const auto& build : info.buildings)
        {
            if(build.Type == BUILDING_CENTER && build.Percent >=100)
            {
                if(build.Project == ACT_NULL
                   && info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD
                   && info.farmers.size() < m_target_farmer_count
                   && info.Human_Num < info.Human_MaxNum)
                {
                    BuildingAction(build.SN, BUILDING_CENTER_CREATEFARMER);
                }
            }
        }

        //2. 统计房屋数量，缺房子就造
        int houseCount =0;
        for(auto& b : info.buildings)
        {
            if(b.Type == BUILDING_HOME && b.Percent >=100) houseCount++;
        }
        int needHouse = (50/4) - houseCount;
        if(needHouse > 0 && info.Wood >= BUILD_HOUSE_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE && farmer.FarmerSort == FARMERTYPE_FARMER)
                {
                    int x,y;
                    if(findFlatBlock(x,y, info))
                    {
                        HumanBuild(farmer.SN, BUILDING_HOME, x, y);
                    }
                    break;
                }
            }
        }

        //3. 更新建筑完成标记
        for(auto &b : info.buildings)
        {
            if(b.Type == BUILDING_STOCK && b.Percent >= 100) m_built_storage = true;
            if(b.Type == BUILDING_GRANARY && b.Percent >=100) m_built_granary = true;
            if(b.Type == BUILDING_ARMYCAMP && b.Percent >=100) m_built_barrack = true;
            if(b.Type == BUILDING_MARKET && b.Percent >=100) m_built_market = true;
            if(b.Type == BUILDING_RANGE && b.Percent >=100) m_built_range = true;
        }

        //建造仓库
        if(!m_built_storage && info.Wood >= BUILD_STOCK_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_STOCK, x,y);
                    }
                    break;
                }
            }
        }
        //建造谷仓
        if(!m_built_granary && info.Wood >= BUILD_GRANARY_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_GRANARY, x,y);
                    }
                    break;
                }
            }
        }
        //建造兵营
        if(!m_built_barrack && info.Wood >= BUILD_ARMYCAMP_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_ARMYCAMP, x,y);
                    }
                    break;
                }
            }
        }
        //建造市场（前置：谷仓建好）
        if(m_built_granary && !m_built_market && info.Wood >= BUILD_MARKET_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_MARKET, x,y);
                    }
                    break;
                }
            }
        }
        //建造靶场（前置：兵营建好）
        if(m_built_barrack && !m_built_range && info.Wood >= BUILD_RANGE_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_RANGE, x,y);
                    }
                    break;
                }
            }
        }

        //4.谷仓研发箭塔科技
        if(m_built_granary && !m_research_arrowtower)
        {
            for(auto &b : info.buildings)
            {
                if(b.Type == BUILDING_GRANARY && b.Percent >= 100 && b.Project == ACT_NULL)
                {
                    BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                    m_research_arrowtower = true;
                    break;
                }
            }
        }

        //5.空闲村民分配采集资源：浆果>树木>石头
        for(auto &farmer : info.farmers)
        {
            if(farmer.NowState != HUMAN_STATE_IDLE)
                continue;
            if(farmer.FarmerSort != FARMERTYPE_FARMER)
                continue;

            int targetSN = -1;
            targetSN = findNearestResource(info, RESOURCE_BUSH, farmer.SN);
            if(targetSN != -1)
            {
                HumanAction(farmer.SN, targetSN);
                continue;
            }
            targetSN = findNearestResource(info, RESOURCE_TREE, farmer.SN);
            if(targetSN != -1)
            {
                HumanAction(farmer.SN, targetSN);
                continue;
            }
            targetSN = findNearestResource(info, RESOURCE_STONE, farmer.SN);
            if(targetSN != -1)
            {
                HumanAction(farmer.SN, targetSN);
                continue;
            }
        }

        //6.市场建好后造农田
        if(m_built_market && info.Wood >= BUILD_FARM_WOOD)
        {
            for(auto& farmer : info.farmers)
            {
                if(farmer.NowState == HUMAN_STATE_IDLE)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(farmer.SN, BUILDING_FARM, x,y);
                    }
                    break;
                }
            }
        }

        //第一阶段完成，切换阶段
        bool finishStage1 = (info.farmers.size() >= m_target_farmer_count)
                && m_built_market
                && m_built_range;
        if(finishStage1)
        {
            m_gameStage = 1;
            DebugText("====第一阶段发育完成，进入防御第一波阶段====");
        }
    }
    else if(m_gameStage ==1)
    {
        //第二阶段代码预留位置，后面再加
    }
}
