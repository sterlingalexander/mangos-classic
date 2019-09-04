/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "WaypointMovementGenerator.h"
#include "Globals/ObjectMgr.h"
#include "Entities/Player.h"
#include "Entities/Creature.h"
#include "AI/BaseAI/CreatureAI.h"
#include "WaypointManager.h"
#include "DBScripts/ScriptMgr.h"
#include "Movement/MoveSplineInit.h"
#include "Movement/MoveSpline.h"

#include <cassert>

//-----------------------------------------------//
void WaypointMovementGenerator<Creature>::LoadPath(Creature& creature, int32 pathId, WaypointPathOrigin wpOrigin, uint32 overwriteEntry)
{
    DETAIL_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "LoadPath: loading waypoint path for %s", creature.GetGuidStr().c_str());

    if (!overwriteEntry)
        overwriteEntry = creature.GetEntry();

    if (wpOrigin == PATH_NO_PATH && pathId == 0)
        i_path = sWaypointMgr.GetDefaultPath(overwriteEntry, creature.GetGUIDLow(), &m_PathOrigin);
    else
    {
        m_PathOrigin = wpOrigin == PATH_NO_PATH ? PATH_FROM_ENTRY : wpOrigin;
        i_path = sWaypointMgr.GetPathFromOrigin(overwriteEntry, creature.GetGUIDLow(), pathId, m_PathOrigin);
    }
    m_pathId = pathId;

    // No movement found for entry nor guid
    if (!i_path)
    {
        if (m_PathOrigin == PATH_FROM_EXTERNAL)
            sLog.outErrorScriptLib("WaypointMovementGenerator::LoadPath: %s doesn't have waypoint path %i", creature.GetGuidStr().c_str(), pathId);
        else
            sLog.outErrorDb("WaypointMovementGenerator::LoadPath: %s doesn't have waypoint path %i", creature.GetGuidStr().c_str(), pathId);
        return;
    }

    if (i_path->empty())
        return;
    // Initialize the i_currentNode to point to the first node
    i_currentNode = i_path->begin()->first;
    m_lastReachedWaypoint = 0;
}

void WaypointMovementGenerator<Creature>::Initialize(Creature& creature)
{
    creature.addUnitState(UNIT_STAT_ROAMING);
    creature.clearUnitState(UNIT_STAT_WAYPOINT_PAUSED);
}

void WaypointMovementGenerator<Creature>::InitializeWaypointPath(Creature& u, int32 pathId, WaypointPathOrigin wpSource, uint32 initialDelay, uint32 overwriteEntry)
{
    LoadPath(u, pathId, wpSource, overwriteEntry);
    i_nextMoveTime.Reset(initialDelay);
    // Start moving if possible
    StartMove(u);
}

void WaypointMovementGenerator<Creature>::Finalize(Creature& creature)
{
    creature.clearUnitState(UNIT_STAT_ROAMING | UNIT_STAT_ROAMING_MOVE);
    creature.SetWalk(!creature.hasUnitState(UNIT_STAT_RUNNING_STATE), false);
}

void WaypointMovementGenerator<Creature>::Interrupt(Creature& creature)
{
    creature.InterruptMoving();
    creature.clearUnitState(UNIT_STAT_ROAMING | UNIT_STAT_ROAMING_MOVE);
    creature.SetWalk(!creature.hasUnitState(UNIT_STAT_RUNNING_STATE), false);
}

void WaypointMovementGenerator<Creature>::Reset(Creature& creature)
{
    creature.addUnitState(UNIT_STAT_ROAMING);
    StartMove(creature);
}

void WaypointMovementGenerator<Creature>::OnArrived(Creature& creature)
{
    if (!i_path || i_path->empty())
        return;

    m_lastReachedWaypoint = i_currentNode;

    if (m_isArrivalDone)
        return;

    creature.clearUnitState(UNIT_STAT_ROAMING_MOVE);
    m_isArrivalDone = true;

    WaypointPath::const_iterator currPoint = i_path->find(i_currentNode);
    MANGOS_ASSERT(currPoint != i_path->end());
    WaypointNode const& node = currPoint->second;

    if (node.script_id)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "Creature movement start script %u at point %u for %s.", node.script_id, i_currentNode, creature.GetGuidStr().c_str());
        creature.GetMap()->ScriptsStart(sCreatureMovementScripts, node.script_id, &creature, &creature);
    }

    // Inform script
    if (creature.AI())
    {
        uint32 type = WAYPOINT_MOTION_TYPE;
        if (m_PathOrigin == PATH_FROM_EXTERNAL)
            type = EXTERNAL_WAYPOINT_MOVE;
        InformAI(creature, type, i_currentNode);
    }

    // Wait delay ms
    Stop(node.delay);
}

void WaypointMovementGenerator<Creature>::StartMove(Creature& creature)
{
    if (!i_path || i_path->empty())
        return;

    if (Stopped(creature))
        return;

    if (!creature.isAlive() || creature.hasUnitState(UNIT_STAT_NOT_MOVE))
        return;

    WaypointPath::const_iterator currPoint = i_path->find(i_currentNode);
    MANGOS_ASSERT(currPoint != i_path->end());

    if (m_isArrivalDone)
    {
        bool reachedLast = false;
        ++currPoint;
        if (currPoint == i_path->end())
        {
            reachedLast = true;
            currPoint = i_path->begin();
        }

        // Inform AI
        if (creature.AI() && m_PathOrigin == PATH_FROM_EXTERNAL)
        {
            uint32 type;
            if (!reachedLast)
                type = EXTERNAL_WAYPOINT_MOVE_START;
            else
                type = EXTERNAL_WAYPOINT_FINISHED_LAST;

            InformAI(creature, type, currPoint->first);

            if (creature.isDead() || !creature.IsInWorld()) // Might have happened with above calls
                return;
        }

        i_currentNode = currPoint->first;
    }

    m_isArrivalDone = false;

    creature.addUnitState(UNIT_STAT_ROAMING_MOVE);

    WaypointNode const& nextNode = currPoint->second;
    Movement::MoveSplineInit init(creature);
    init.MoveTo(nextNode.x, nextNode.y, nextNode.z, true);

    if (nextNode.orientation != 100 && nextNode.delay != 0)
        init.SetFacing(nextNode.orientation);
    creature.SetWalk(!creature.hasUnitState(UNIT_STAT_RUNNING_STATE) && !creature.IsLevitating(), false);
    init.Launch();
}

void WaypointMovementGenerator<Creature>::InformAI(Creature& creature, uint32 type, uint32 data)
{
    creature.AI()->MovementInform(type, data);
    if (creature.IsTemporarySummon())
    {
        if (creature.GetSpawnerGuid().IsCreatureOrPet())
            if (Creature* pSummoner = creature.GetMap()->GetAnyTypeCreature(creature.GetSpawnerGuid()))
                if (pSummoner->AI())
                    pSummoner->AI()->SummonedMovementInform(&creature, type, data);
    }
}

bool WaypointMovementGenerator<Creature>::Update(Creature& creature, const uint32& diff)
{
    // Waypoint movement can be switched on/off
    // This is quite handy for escort quests and other stuff
    if (creature.hasUnitState(UNIT_STAT_NOT_MOVE))
    {
        creature.clearUnitState(UNIT_STAT_ROAMING_MOVE);
        return true;
    }

    // prevent a crash at empty waypoint path.
    if (!i_path || i_path->empty())
    {
        creature.clearUnitState(UNIT_STAT_ROAMING_MOVE);
        return true;
    }

    if (Stopped(creature))
    {
        if (CanMove(diff, creature))
            StartMove(creature);
    }
    else
    {
        if (creature.IsStopped())
            Stop(STOP_TIME_FOR_PLAYER);
        else if (creature.movespline->Finalized())
        {
            OnArrived(creature);
            StartMove(creature);
        }
    }
    return true;
}

bool WaypointMovementGenerator<Creature>::Stopped(Creature& u)
{
    return !i_nextMoveTime.Passed() || u.hasUnitState(UNIT_STAT_WAYPOINT_PAUSED);
}

bool WaypointMovementGenerator<Creature>::CanMove(int32 diff, Creature& u)
{
    i_nextMoveTime.Update(diff);
    if (i_nextMoveTime.Passed() && u.hasUnitState(UNIT_STAT_WAYPOINT_PAUSED))
        i_nextMoveTime.Reset(1);

    return i_nextMoveTime.Passed() && !u.hasUnitState(UNIT_STAT_WAYPOINT_PAUSED);
}

bool WaypointMovementGenerator<Creature>::GetResetPosition(Creature&, float& x, float& y, float& z, float& o) const
{
    // prevent a crash at empty waypoint path.
    if (!i_path || i_path->empty())
        return false;

    WaypointPath::const_iterator lastPoint = i_path->find(m_lastReachedWaypoint);
    // Special case: Before the first waypoint is reached, m_lastReachedWaypoint is set to 0 (which may not be contained in i_path)
    if (!m_lastReachedWaypoint && lastPoint == i_path->end())
        return false;

    MANGOS_ASSERT(lastPoint != i_path->end());

    WaypointNode const* curWP = &(lastPoint->second);

    x = curWP->x;
    y = curWP->y;
    z = curWP->z;

    if (curWP->orientation != 100)
        o = curWP->orientation;
    else                                                    // Calculate the resulting angle based on positions between previous and current waypoint
    {
        WaypointNode const* prevWP;
        if (lastPoint != i_path->begin())                   // Not the first waypoint
        {
            --lastPoint;
            prevWP = &(lastPoint->second);
        }
        else                                                // Take the last waypoint (crbegin()) as previous
            prevWP = &(i_path->rbegin()->second);

        float dx = x - prevWP->x;
        float dy = y - prevWP->y;
        o = atan2(dy, dx);                                  // returns value between -Pi..Pi

        o = (o >= 0) ? o : 2 * M_PI_F + o;
    }

    return true;
}

void WaypointMovementGenerator<Creature>::GetPathInformation(std::ostringstream& oss) const
{
    oss << "WaypointMovement: Last Reached WP: " << m_lastReachedWaypoint << " ";
    oss << "(Loaded path " << m_pathId << " from " << WaypointManager::GetOriginString(m_PathOrigin) << ")\n";
}

void WaypointMovementGenerator<Creature>::AddToWaypointPauseTime(int32 waitTimeDiff)
{
    if (!i_nextMoveTime.Passed())
    {
        // Prevent <= 0, the code in Update requires to catch the change from moving to not moving
        int32 newWaitTime = i_nextMoveTime.GetExpiry() + waitTimeDiff;
        i_nextMoveTime.Reset(newWaitTime > 0 ? newWaitTime : 1);
    }
}

bool WaypointMovementGenerator<Creature>::SetNextWaypoint(uint32 pointId)
{
    if (!i_path || i_path->empty())
        return false;

    WaypointPath::const_iterator currPoint = i_path->find(pointId);
    if (currPoint == i_path->end())
        return false;

    // Allow Moving with next tick
    // Handle allow movement this way to not interact with PAUSED state.
    // If this function is called while PAUSED, it will move properly when unpaused.
    i_nextMoveTime.Reset(1);
    m_isArrivalDone = false;

    // Set the point
    i_currentNode = pointId;
    return true;
}

//----------------------------------------------------//

void FlightPathMovementGenerator::Initialize(Player& player)
{
    player.InterruptMoving();
    player.addUnitState(UNIT_STAT_TAXI_FLIGHT);
    player.SetFlag(UNIT_FIELD_FLAGS, (UNIT_FLAG_CLIENT_CONTROL_LOST | UNIT_FLAG_TAXI_FLIGHT));
    player.UpdateClientControl(&player, false);
}

void FlightPathMovementGenerator::Finalize(Player& player)
{
    player.InterruptMoving();
    player.clearUnitState(UNIT_STAT_TAXI_FLIGHT);
    player.RemoveFlag(UNIT_FIELD_FLAGS, (UNIT_FLAG_CLIENT_CONTROL_LOST | UNIT_FLAG_TAXI_FLIGHT));
    player.UpdateClientControl(&player, true);
}

void FlightPathMovementGenerator::Interrupt(Player& player)
{
    player.InterruptMoving();
    player.clearUnitState(UNIT_STAT_TAXI_FLIGHT);
}

void FlightPathMovementGenerator::Reset(Player& player)
{
    Initialize(player);
}

#define PLAYER_FLIGHT_SPEED        32.0f

bool FlightPathMovementGenerator::Update(Player& player, const uint32& /*diff*/)
{
    Movement::MoveSpline* spline = player.movespline;
    const bool movement = (!spline->Finalized());

    if (!player.OnTaxiFlightUpdate(size_t(spline->currentPathIdx()), movement))
    {
        // Some other code messed with our spline before it was completed. Have it your way, no taxi for you!
        player.OnTaxiFlightEject();
        return false;
    }

    // We are waiting for spline to complete at this point
    if (movement)
        return true;
    if (player.IsMounted())
        player.OnTaxiFlightSplineEnd();

    // Load and execute the spline
    if (player.OnTaxiFlightSplineUpdate())
    {
        auto nodes = player.GetTaxiPathSpline();
        auto offset = player.GetTaxiSplinePathOffset();
        Movement::MoveSplineInit init(player);
        Movement::PointsArray& path = init.Path();
        for (auto i = offset; i < nodes.size(); ++i)
        {
            auto node = nodes.at(i);
            path.push_back({ node->x, node->y, node->z });
        }
        init.SetFirstPointId(int32(offset));
        init.SetFly();
        init.SetVelocity(PLAYER_FLIGHT_SPEED);
        init.Launch();
        return true;
    }
    return false;
}

bool FlightPathMovementGenerator::Resume(Player& player) const
{
    if (player.IsMounted())
        player.OnTaxiFlightSplineEnd();

    // Load and execute the spline
    if (player.OnTaxiFlightSplineUpdate())
    {
        auto nodes = player.GetTaxiPathSpline();
        auto offset = player.GetTaxiSplinePathOffset();
        Movement::MoveSplineInit init(player);
        Movement::PointsArray& path = init.Path();
        for (auto i = offset; i < nodes.size(); ++i)
        {
            auto node = nodes.at(i);
            path.push_back({ node->x, node->y, node->z });
        }
        init.SetFirstPointId(int32(offset));
        init.SetFly();
        init.SetVelocity(PLAYER_FLIGHT_SPEED);
        init.Launch();
        return true;
    }
    return false;
}
