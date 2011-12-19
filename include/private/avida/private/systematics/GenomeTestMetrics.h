/*
 *  private/systematics/GenomeTestMetrics.h
 *  Avida
 *
 *  Created by David Bryson on 8/13/10.
 *  Copyright 2010-2011 Michigan State University. All rights reserved.
 *
 *
 *  This file is part of Avida.
 *
 *  Avida is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 *  Avida is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License along with Avida.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Authors: David M. Bryson <david@programerror.com>
 *
 */

#ifndef AvidaSystematicsGenomeTestMetrics_h
#define AvidaSystematicsGenomeTestMetrics_h

#include "apto/platform.h"
#include "avida/systematics/Group.h"

class cAvidaContext;
class cWorld;


namespace Avida {
  namespace Systematics {
        
    // Class Declaractions
    // --------------------------------------------------------------------------------------------------------------
    
    class GenomeTestMetrics;
    
    
    // Type Declarations
    // --------------------------------------------------------------------------------------------------------------
    
    typedef Apto::SmartPtr<GenomeTestMetrics> GenomeTestMetricsPtr;
    
        
    // GenomeTestMetrics
    // --------------------------------------------------------------------------------------------------------------
    
    class GenomeTestMetrics : public GroupData
    {
    private:
      bool m_is_viable;
      double m_fitness;
      double m_colony_fitness;
      double m_merit;
      int m_copied_size;
      int m_executed_size;
      int m_gestation_time;
      
      
      LIB_LOCAL GenomeTestMetrics(cWorld* world, cAvidaContext& ctx, GroupPtr bg);
      
    public:
      LIB_LOCAL bool Serialize(ArchivePtr ar) const;
      
      LIB_LOCAL bool IsViable() const { return m_is_viable; }
      LIB_LOCAL double GetFitness() const { return m_fitness; }
      LIB_LOCAL double GetColonyFitness() const { return m_colony_fitness; }
      LIB_LOCAL double GetMerit() const { return m_merit; }
      LIB_LOCAL int GetLinesCopied() const { return m_copied_size; }
      LIB_LOCAL int GetLinesExecuted() const { return m_executed_size; }
      LIB_LOCAL int GetGestationTime() const { return m_gestation_time; }
      
      
      LIB_LOCAL static GenomeTestMetricsPtr GetMetrics(cWorld* world, cAvidaContext& ctx, GroupPtr bg);
    };
    
  };
};

#endif
