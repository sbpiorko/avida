/*
 *  cPopulation.cc
 *  Avida
 *
 *  Called "population.cc" prior to 12/5/05.
 *  Copyright 2005-2006 Michigan State University. All rights reserved.
 *  Copyright 1993-2003 California Institute of Technology.
 *
 */

#include "cPopulation.h"

#include "cAvidaContext.h"
#include "cChangeList.h"
#include "cClassificationManager.h"
#include "cCodeLabel.h"
#include "cConstSchedule.h"
#include "cDataFile.h"
#include "cEnvironment.h"
#include "functions.h"
#include "cGenomeUtil.h"
#include "cGenotype.h"
#include "cHardwareBase.h"
#include "cHardwareManager.h"
#include "cInitFile.h"
#include "cInjectGenotype.h"
#include "cInstUtil.h"
#include "cIntegratedSchedule.h"
#include "cLineage.h"
#include "cOrganism.h"
#include "cPhenotype.h"
#include "cPopulationCell.h"
#include "cProbSchedule.h"
#include "cResource.h"
#include "cSpecies.h"
#include "cStats.h"
#include "cTaskEntry.h"
#include "cWorld.h"

#include <fstream>
#include <vector>
#include <algorithm>
#include <set>

#include <float.h>
#include <math.h>

using namespace std;


cPopulation::cPopulation(cWorld* world)
: m_world(world)
, schedule(NULL)
, resource_count(world->GetEnvironment().GetResourceLib().GetSize())
, birth_chamber(world)
, environment(world->GetEnvironment())
, num_organisms(0)
, sync_events(false)
{
  // Avida specific information.
  world_x = world->GetConfig().WORLD_X.Get();
  world_y = world->GetConfig().WORLD_Y.Get();
  int geometry = world->GetConfig().WORLD_GEOMETRY.Get();
  const int num_cells = world_x * world_y;

  // Print out world details
  if (world->GetVerbosity() > VERBOSE_NORMAL) {
    cout << "Building world " << world_x << "x" << world_y << " = " << num_cells << " organisms." << endl;
    if (geometry == nGeometry::GRID) {
      cout << "Geometry: Bounded grid" << endl;
    } else if (geometry == nGeometry::TORUS) {
      cout << "Geometry: Torus" << endl;
    } else {
      cout << "Geometry: Unknown" << endl;
    }
    cout << endl;
  }
  
  cell_array.Resize(num_cells);
  resource_count.ResizeSpatialGrids(world_x, world_y);
  market.Resize(MARKET_SIZE);
  
  for (int cell_id = 0; cell_id < num_cells; cell_id++) {
    int x = cell_id % world_x;
    int y = cell_id / world_x;
    cell_array[cell_id].Setup(world, cell_id, environment.GetMutRates());
    
    // If we're working with a bounded grid, we need to take care of it.
    bool bottom_flag = true;
    bool top_flag = true;
    bool right_flag = true;
    bool left_flag = true;

    if (geometry == nGeometry::GRID) {
      if (y == 0)  bottom_flag = false;
      if (y == world_y-1)  top_flag = false;
      if (x == 0) left_flag = false;
      if (x == world_x-1) right_flag = false;
    }

    // Setup the connection list for each cell. (Clockwise from -1 to 1)
    
    tList<cPopulationCell> & conn_list=cell_array[cell_id].ConnectionList();
    if (bottom_flag && left_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, -1, -1)]));
    }
    if (bottom_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y,  0, -1)]));
    }
    if (bottom_flag && right_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, +1, -1)]));
    }
    if (right_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, +1,  0)]));
    }
    if (top_flag && right_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, +1, +1)]));
    }
    if (top_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y,  0, +1)]));
    }
    if (top_flag && left_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, -1, +1)]));
    }
    if (left_flag) {
      conn_list.Push(&(cell_array[GridNeighbor(cell_id,world_x,world_y, -1,  0)]));
    }

    // Setup the reaper queue...
    if (world->GetConfig().BIRTH_METHOD.Get() == POSITION_CHILD_FULL_SOUP_ELDEST) {
      reaper_queue.Push(&(cell_array[cell_id]));
    }
  }
  
  BuildTimeSlicer(0);
  
  if (SetupDemes() == false) {
    cerr << "Error: Failed to setup demes.  Exiting..." << endl;
    exit(1);
  }
  
  // Setup the resources...
  const cResourceLib & resource_lib = environment.GetResourceLib();
  for (int i = 0; i < resource_lib.GetSize(); i++) {
    cResource * res = resource_lib.GetResource(i);
    const double decay = 1.0 - res->GetOutflow();
    resource_count.Setup(i, res->GetName(), res->GetInitial(), 
                         res->GetInflow(), decay,
                         res->GetGeometry(), res->GetXDiffuse(),
                         res->GetXGravity(), res->GetYDiffuse(), 
                         res->GetYGravity(), res->GetInflowX1(), 
                         res->GetInflowX2(), res->GetInflowY1(), 
                         res->GetInflowY2(), res->GetOutflowX1(), 
                         res->GetOutflowX2(), res->GetOutflowY1(), 
                         res->GetOutflowY2(), world->GetVerbosity() );
    m_world->GetStats().SetResourceName(i, res->GetName());
  }
 
  // Give stats information about the environment...
  const cTaskLib & task_lib = environment.GetTaskLib();
  for (int i = 0; i < task_lib.GetSize(); i++) {
    const cTaskEntry & cur_task = task_lib.GetTask(i);
    m_world->GetStats().SetTaskName(i, cur_task.GetDesc());
  }
  
  const cInstSet & inst_set = world->GetHardwareManager().GetInstSet();
  for (int i = 0; i < inst_set.GetSize(); i++) {
    m_world->GetStats().SetInstName(i, inst_set.GetName(i));
  }

  // Load a clone if one is provided, otherwise setup start organism.
  if (m_world->GetConfig().CLONE_FILE.Get() == "-" || m_world->GetConfig().CLONE_FILE.Get() == "") {
    cGenome start_org = cInstUtil::LoadGenome(m_world->GetConfig().START_CREATURE.Get(), world->GetHardwareManager().GetInstSet());
    if (start_org.GetSize() != 0) Inject(start_org);
    else cerr << "Warning: Zero length start organism, not injecting into initial population." << endl;
  } else {
    ifstream fp(m_world->GetConfig().CLONE_FILE.Get());
    LoadClone(fp);
  }
}


cPopulation::~cPopulation()
{
  for (int i = 0; i < cell_array.GetSize(); i++) KillOrganism(cell_array[i]);
  delete schedule;
}


// This method configures demes in the population.  Demes are subgroups of
// organisms evolved together and used in group selection experiments.

bool cPopulation::SetupDemes()
{
  const int num_demes = m_world->GetConfig().NUM_DEMES.Get();
  const int birth_method = m_world->GetConfig().BIRTH_METHOD.Get();
  
  // If we are not using demes, stop here.
  if (num_demes == 0) {
    if (birth_method == POSITION_CHILD_DEME_RANDOM) {
      cerr << "Using position method that requires demes, but demes are off."
      << endl;
      return false;
    }
    return true;
  }
  
  deme_array.Resize(num_demes);

  // Check to make sure all other settings are reasonable to have demes.
  // ...make sure populaiton can be divided up evenly.
  if (world_y % num_demes != 0) {
    cerr << "World Y size of " << world_y
    << " cannot be divided into " << num_demes << " demes." << endl;
    return false;
  }
  
  // ...make sure we are using a legal birth method.
  if (birth_method == POSITION_CHILD_FULL_SOUP_ELDEST ||
      birth_method == POSITION_CHILD_FULL_SOUP_RANDOM) {
    cerr << "Illegal birth method " << birth_method << " for use with demes." << endl;
    return false;
  }
  
  const int deme_size_x = world_x;
  const int deme_size_y = world_y / num_demes;
  const int deme_size = deme_size_x * deme_size_y;
  
  
  // Setup the deme structures.
  tArray<int> deme_cells(deme_size);
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    for (int offset = 0; offset < deme_size; offset++) {
      int cell_id = deme_id * deme_size + offset;
      deme_cells[offset] = cell_id;
      cell_array[cell_id].SetDemeID(deme_id);
    }
    deme_array[deme_id].Setup(deme_cells);
  }

  
  // Build walls in the population.
  for (int row_id = 0; row_id < world_y; row_id += deme_size_y) {
    // Loop through all of the cols and make the cut on each...
    for (int col_id = 0; col_id < world_x; col_id++) {
      int idA = row_id * world_x + col_id;
      int idB  = GridNeighbor(idA, world_x, world_y,  0, -1);
      int idA0 = GridNeighbor(idA, world_x, world_y, -1,  0);
      int idA1 = GridNeighbor(idA, world_x, world_y,  1,  0);
      int idB0 = GridNeighbor(idA, world_x, world_y, -1, -1);
      int idB1 = GridNeighbor(idA, world_x, world_y,  1, -1);
      cPopulationCell & cellA = GetCell(idA);
      cPopulationCell & cellB = GetCell(idB);
      tList<cPopulationCell> & cellA_list = cellA.ConnectionList();
      tList<cPopulationCell> & cellB_list = cellB.ConnectionList();
      cellA_list.Remove(&GetCell(idB));
      cellA_list.Remove(&GetCell(idB0));
      cellA_list.Remove(&GetCell(idB1));
      cellB_list.Remove(&GetCell(idA));
      cellB_list.Remove(&GetCell(idA0));
      cellB_list.Remove(&GetCell(idA1));
    }
  }
  
  return true;
}

// Activate the child, given information from the parent.
// Return true if parent lives through this process.

bool cPopulation::ActivateOffspring(cAvidaContext& ctx, cGenome& child_genome, cOrganism& parent_organism)
{
  assert(&parent_organism != NULL);
  
  tArray<cOrganism*> child_array;
  tArray<cMerit> merit_array;
  
  // Update the parent's phenotype.
  // This needs to be done before the parent goes into the brith chamber
  // or the merit doesn't get passed onto the child correctly
  cPhenotype& parent_phenotype = parent_organism.GetPhenotype();
  parent_phenotype.DivideReset(parent_organism.GetGenome().GetSize());
  
  birth_chamber.SubmitOffspring(ctx, child_genome, parent_organism, child_array, merit_array);
  
  // First, setup the genotype of all of the offspring.
  cGenotype* parent_genotype = parent_organism.GetGenotype();
  const int parent_id = parent_organism.GetOrgInterface().GetCellID();
  assert(parent_id >= 0 && parent_id < cell_array.GetSize());
  cPopulationCell& parent_cell = cell_array[parent_id];
    
  tArray<int> target_cells(child_array.GetSize());
  
  // Loop through choosing the later placement of each child in the population.
  bool parent_alive = true;  // Will the parent live through this process?
  for (int i = 0; i < child_array.GetSize(); i++) {
    target_cells[i] = PositionChild(parent_cell).GetID();
    
    // If we replaced the parent, make a note of this.
    if (target_cells[i] == parent_cell.GetID()) parent_alive = false;      
    
    // Update the mutation rates of each child....
    child_array[i]->MutationRates().Copy(GetCell(target_cells[i]).MutationRates());
    
    // Update the phenotypes of each child....
    const int child_length = child_array[i]->GetGenome().GetSize();
    child_array[i]->GetPhenotype().SetupOffspring(parent_phenotype,child_length);
    
    child_array[i]->GetPhenotype().SetMerit(merit_array[i]);
    
    // Do lineage tracking for the new organisms.
    LineageSetupOrganism(child_array[i], parent_organism.GetLineage(),
                         parent_organism.GetLineageLabel(), parent_genotype);
    
  }
  
  
  // If we're not about to kill the parent, do some extra work on it.
  if (parent_alive == true) {
    schedule->Adjust(parent_cell.GetID(), parent_phenotype.GetMerit());
    
    // In a local run, face the child toward the parent. 
    const int birth_method = m_world->GetConfig().BIRTH_METHOD.Get();
    if (birth_method < NUM_LOCAL_POSITION_CHILD ||
	birth_method == POSITION_CHILD_PARENT_FACING) {
      for (int i = 0; i < child_array.GetSize(); i++) {
        GetCell(target_cells[i]).Rotate(parent_cell);
      }
    }
  }
  
  // Do any statistics on the parent that just gave birth...
  parent_genotype->AddGestationTime( parent_phenotype.GetGestationTime() );
  parent_genotype->AddFitness(       parent_phenotype.GetFitness()       );
  parent_genotype->AddMerit(         parent_phenotype.GetMerit()         );
  parent_genotype->AddCopiedSize(    parent_phenotype.GetCopiedSize()    );
  parent_genotype->AddExecutedSize(  parent_phenotype.GetExecutedSize()  );
  
  // Place all of the offspring...
  for (int i = 0; i < child_array.GetSize(); i++) {
    ActivateOrganism(ctx, child_array[i], GetCell(target_cells[i]));
    cGenotype* child_genotype = child_array[i]->GetGenotype();
    child_genotype->DecDeferAdjust();
    m_world->GetClassificationManager().AdjustGenotype(*child_genotype);
  }
  
  return parent_alive;
}

bool cPopulation::ActivateParasite(cOrganism& parent, const cGenome& injected_code)
{
  assert(&parent != NULL);
  
  if (injected_code.GetSize() == 0) return false;
  
  cHardwareBase& parent_cpu = parent.GetHardware();
  cInjectGenotype* parent_genotype = parent_cpu.ThreadGetOwner();
  
  const int parent_id = parent.GetOrgInterface().GetCellID();
  assert(parent_id >= 0 && parent_id < cell_array.GetSize());
  cPopulationCell& parent_cell = cell_array[ parent_id ];
  
  int num_neighbors = parent.GetNeighborhoodSize();
  cOrganism* target_organism = 
    parent_cell.connection_list.GetPos(m_world->GetRandom().GetUInt(num_neighbors))->GetOrganism();
  
  if (target_organism == NULL) return false;
  
  cHardwareBase& child_cpu = target_organism->GetHardware();
  
  if (child_cpu.GetNumThreads() == m_world->GetConfig().MAX_CPU_THREADS.Get()) return false;
  
  
  if (target_organism->InjectHost(parent_cpu.GetLabel(), injected_code)) {
    cInjectGenotype* child_genotype = parent_genotype;

    // If the parent genotype is not correct for the child, adjust it.
    if (parent_genotype == NULL || parent_genotype->GetGenome() != injected_code) {
      child_genotype = m_world->GetClassificationManager().GetInjectGenotype(injected_code, parent_genotype);
    }
    
    target_organism->AddParasite(child_genotype);
    child_genotype->AddParasite();
    child_cpu.ThreadSetOwner(child_genotype);
    m_world->GetClassificationManager().AdjustInjectGenotype(*child_genotype);
  }
  else
    return false;
  
  return true;
}

void cPopulation::ActivateOrganism(cAvidaContext& ctx, cOrganism* in_organism, cPopulationCell& target_cell)
{
  assert(in_organism != NULL);
  assert(in_organism->GetGenome().GetSize() > 1);
  
  in_organism->SetOrgInterface(new cPopulationInterface(m_world));
  
  // If the organism does not have a genotype, give it one!  No parent
  // information is provided so we must set parents to NULL.
  if (in_organism->GetGenotype() == NULL) {
    cGenotype* new_genotype = m_world->GetClassificationManager().GetGenotype(in_organism->GetGenome(), NULL, NULL);
    in_organism->SetGenotype(new_genotype);
  }
  cGenotype* in_genotype = in_organism->GetGenotype();
  
  // Save the old genotype from this cell...
  cGenotype* old_genotype = NULL;
  if (target_cell.IsOccupied()) {
    old_genotype = target_cell.GetOrganism()->GetGenotype();
    
    // Sometimes a new organism will kill off the last member of its genotype
    // in the population.  Normally this would remove the genotype, so we 
    // want to defer adjusting that genotype until the new one is placed.
    old_genotype->IncDeferAdjust();
  }
  
  // Update the contents of the target cell.
  KillOrganism(target_cell);
  target_cell.InsertOrganism(*in_organism);

  // Setup the inputs in the target cell.
  environment.SetupInputs(ctx, target_cell.input_array);
  
  // Update the archive...
  in_genotype->AddOrganism();
  
  if (old_genotype != NULL) {
    old_genotype->DecDeferAdjust();
    m_world->GetClassificationManager().AdjustGenotype(*old_genotype);
  }
  m_world->GetClassificationManager().AdjustGenotype(*in_genotype);
  
  // Initialize the time-slice for this new organism.
  schedule->Adjust(target_cell.GetID(), in_organism->GetPhenotype().GetMerit());
  
  // Special handling for certain birth methods.
  if (m_world->GetConfig().BIRTH_METHOD.Get() == POSITION_CHILD_FULL_SOUP_ELDEST) {
    reaper_queue.Push(&target_cell);
  }
  
  // Keep track of statistics for organism counts...
  num_organisms++;
  if (deme_array.GetSize() > 0) {
    deme_array[target_cell.GetDemeID()].IncOrgCount();
  }
  
  // Statistics...
  m_world->GetStats().RecordBirth(target_cell.GetID(), in_genotype->GetID(),
                                  in_organism->GetPhenotype().ParentTrue());
}

void cPopulation::KillOrganism(cPopulationCell& in_cell)
{
  // do we actually have something to kill?
  if (in_cell.IsOccupied() == false) return;
  
  // Statistics...
  cOrganism* organism = in_cell.GetOrganism();
  cGenotype* genotype = organism->GetGenotype();
  m_world->GetStats().RecordDeath();

  tList<tListNode<cSaleItem> >* sold_items = organism->GetSoldItems();
  if (sold_items)
  {
	  tListIterator<tListNode<cSaleItem> > sold_it(*sold_items);
	  tListNode<cSaleItem> * test_node;

	  while ( (test_node = sold_it.Next()) != NULL)
	  {
		tListIterator<cSaleItem> market_it(market[test_node->data->GetLabel()]);
		market_it.Set(test_node);
		delete market_it.Remove();
	  }
  }
  // Do the lineage handling
  if (m_world->GetConfig().LOG_LINEAGES.Get()) { m_world->GetClassificationManager().RemoveLineageOrganism(organism); }
  
  // Update count statistics...
  num_organisms--;
  if (deme_array.GetSize() > 0) {
    deme_array[in_cell.GetDemeID()].DecOrgCount();
  }
  genotype->RemoveOrganism();

  for (int i = 0; i < organism->GetNumParasites(); i++) {
    organism->GetParasite(i).RemoveParasite();
  }

  // And clear it!
  in_cell.RemoveOrganism();
  if (!organism->GetIsRunning()) delete organism;
  else organism->GetPhenotype().SetToDelete();

  // Alert the scheduler that this cell has a 0 merit.
  schedule->Adjust(in_cell.GetID(), cMerit(0));

  // Update the archive (note: genotype adjustment may be defered)
  m_world->GetClassificationManager().AdjustGenotype(*genotype);
}

void cPopulation::Kaboom(cPopulationCell & in_cell, int distance)
{
  cOrganism * organism = in_cell.GetOrganism();
  cGenotype * genotype = organism->GetGenotype();
  cGenome genome = genotype->GetGenome();
  int id = genotype->GetID();
  
  int radius = 2;
  int count = 0;
  
  for (int i=-1*radius; i<=radius; i++) {
    for (int j=-1*radius; j<=radius; j++) {
      cPopulationCell & death_cell =
      cell_array[GridNeighbor(in_cell.GetID(), world_x, world_y, i, j)];
      //do we actually have something to kill?
      if (death_cell.IsOccupied() == false) continue;
      
      cOrganism * org_temp = death_cell.GetOrganism();
      cGenotype * gene_temp = org_temp->GetGenotype();
      
      if (distance == 0) {
        int temp_id = gene_temp->GetID();
        if (temp_id != id) {
          KillOrganism(death_cell);
          count++;
        }
      }
      else {	
        cGenome genome_temp = gene_temp->GetGenome();
        int diff=0;
        for (int i=0; i<genome_temp.GetSize(); i++)
          if (genome_temp.AsString()[i] != genome.AsString()[i])
            diff++;
        if (diff > distance)
        {
          KillOrganism(death_cell);
          count++;
        }
      }
    }
  }
  KillOrganism(in_cell);
  // @SLG my prediction = 92% and, 28 get equals
}

void cPopulation::AddSellValue(const int data, const int label, const int sell_price, const int org_id, const int cell_id)
{
	// find list under appropriate label, labels more than 8 nops long are simply the same
	// as a smaller label modded by the market size
	//int pos = label % market.GetSize();

	//// id of genotype currently residing in cell that seller live(d) in compared to 
	//// id of genotype of actual seller, if different than seller is dead, remove item from list
	//while ( market[pos].GetSize() > 0 && 
	//	(!GetCell(market[pos].GetFirst()->GetCellID()).IsOccupied() ||
	//	GetCell(market[pos].GetFirst()->GetCellID()).GetOrganism()->GetID()
	//	!= 	market[pos].GetFirst()->GetOrgID()) )
	//{
	//	market[pos].Pop();
	//}

	// create sale item
	cSaleItem *new_item = new cSaleItem(data, label, sell_price, org_id, cell_id);

	// place into array by label, array is big enough for labels up to 8 nops long
	tListNode<cSaleItem>* sell_node = market[label].PushRear(new_item);
	tListNode<tListNode<cSaleItem> >* org_node = GetCell(cell_id).GetOrganism()->AddSoldItem(sell_node);
	sell_node->data->SetNodePtr(org_node);

	//:7 for Kolby
}

int cPopulation::BuyValue(const int label, const int buy_price, const int cell_id)
{
	// find list under appropriate label, labels more than 8 nops long are simply the same
	// as a smaller label modded by the market size
	//int pos = label % market.GetSize();

	//// id of genotype currently residing in cell that seller live(d) in compared to 
	//// id of genotype of actual seller, if different than seller is dead, remove item from list
	//while ( market[pos].GetSize() > 0 && 
	//	(!GetCell(market[pos].GetFirst()->GetCellID()).IsOccupied() ||
	//	GetCell(market[pos].GetFirst()->GetCellID()).GetOrganism()->GetID()
	//	!= 	market[pos].GetFirst()->GetOrgID()) )
	//{
	//	market[pos].Pop();
	//}

	// if there's nothing in the list don't bother with rest
	if (market[label].GetSize() <= 0)
		return 0;

	// if the sell price is higher than we're willing to pay no purchase made
	if (market[label].GetFirst()->GetPrice() > buy_price)
		return 0;

	// if the buy price is higher than buying org's current merit no purchase made
	if (GetCell(cell_id).GetOrganism()->GetPhenotype().GetMerit().GetDouble() < buy_price)
		return 0;

	// otherwise transaction should be completed!
	cSaleItem* chosen = market[label].Pop();
	tListIterator<tListNode<cSaleItem> > sold_it(*GetCell(chosen->GetCellID()).GetOrganism()->GetSoldItems());
	sold_it.Set(chosen->GetNodePtr());
	sold_it.Remove();

	// first update sellers merit
	double cur_merit = GetCell(chosen->GetCellID()).GetOrganism()->GetPhenotype().GetMerit().GetDouble();
	cur_merit += buy_price;
	
	GetCell(chosen->GetCellID()).GetOrganism()->UpdateMerit(cur_merit);
	
	// next remove sold item from list in market 
	//market[pos].Remove(chosen);


	// finally return recieve value, buyer merit will be updated if return a valid value here
	int receive_value = chosen->GetData();
	return receive_value;
}

// CompeteDemes  probabilistically copies demes into the next generation
// based on their fitness. How deme fitness is estimated is specified by 
// competition_type input argument as:
/*
 0: deme fitness = 1 (control, random deme selection)
 1: deme fitness = number of births since last competition (default) 
 2: deme fitness = average organism fitness at the current update (uses parent's fitness, so
                     does not work with donations)
 3: deme fitness = average mutation rate at the current update
 4: deme fitness = strong rank selection on (parents) fitness (2^-deme fitness rank)
 5: deme fitness = average organism life (current, not parents) fitness (works with donations)
 6: deme fitness = strong rank selection on life (current, not parents) fitness
*/
//  For ease of use, each organism 
// is setup as if it we just injected into the population.

void cPopulation::CompeteDemes(int competition_type)
{
  const int num_demes = deme_array.GetSize();
  
  double total_fitness = 0; 
  tArray<double> deme_fitness(num_demes); 
  
  switch(competition_type) {
    case 0:    // deme fitness = 1; 
      total_fitness = (double) num_demes;
      deme_fitness.SetAll(1); 
      break; 
    case 1:     // deme fitness = number of births
                // Determine the scale for fitness by totaling births across demes.
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
	double cur_fitness = (double) deme_array[deme_id].GetBirthCount();
        deme_fitness[deme_id] = cur_fitness;
        total_fitness += cur_fitness;
      }
      break; 
    case 2:    // deme fitness = average organism fitness at the current update
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        cDoubleSum single_deme_fitness;
	const cDeme & cur_deme = deme_array[deme_id];
        for (int i = 0; i < cur_deme.GetSize(); i++) {
          int cur_cell = cur_deme.GetCellID(i);
          if (cell_array[cur_cell].IsOccupied() == false) continue;
          cPhenotype & phenotype =
	    GetCell(cur_cell).GetOrganism()->GetPhenotype();
          single_deme_fitness.Add(phenotype.GetFitness());
        } 
        deme_fitness[deme_id] = single_deme_fitness.Ave();
        total_fitness += deme_fitness[deme_id];
      }
      break; 
    case 3: 	// deme fitness = average mutation rate at the current update 
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        cDoubleSum single_deme_div_type;
	const cDeme & cur_deme = deme_array[deme_id];
        for (int i = 0; i < cur_deme.GetSize(); i++) {
          int cur_cell = cur_deme.GetCellID(i);
          if (cell_array[cur_cell].IsOccupied() == false) continue;
          cPhenotype & phenotype =
	    GetCell(cur_cell).GetOrganism()->GetPhenotype();
          assert(phenotype.GetDivType()>0);
          single_deme_div_type.Add(1/phenotype.GetDivType());
        }
        deme_fitness[deme_id] = single_deme_div_type.Ave();
        total_fitness += deme_fitness[deme_id];
      }
      break; 
    case 4: 	// deme fitness = 2^(-deme fitness rank) 
              // first find all the deme fitness values ...
    {      
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        cDoubleSum single_deme_fitness;
	const cDeme & cur_deme = deme_array[deme_id];
        for (int i = 0; i < cur_deme.GetSize(); i++) {
          int cur_cell = cur_deme.GetCellID(i);
          if (cell_array[cur_cell].IsOccupied() == false) continue;
          cPhenotype & phenotype = GetCell(cur_cell).GetOrganism()->GetPhenotype();
          single_deme_fitness.Add(phenotype.GetFitness());
        }  
        deme_fitness[deme_id] = single_deme_fitness.Ave();
      }
      // ... then determine the rank of each deme based on its fitness
      tArray<double> deme_rank(num_demes);
      deme_rank.SetAll(1);
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        for (int test_deme = 0; test_deme < num_demes; test_deme++) {
          if (deme_fitness[deme_id] < deme_fitness[test_deme]) {
            deme_rank[deme_id]++;
          } 
        } 
      } 
      // ... finally, make deme fitness 2^(-deme rank)
      deme_fitness.SetAll(1);	
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        for (int i = 0; i < deme_rank[deme_id]; i++) { 
          deme_fitness[deme_id] = deme_fitness[deme_id]/2;
        } 
        total_fitness += deme_fitness[deme_id]; 
      } 
    }
    break; 
  case 5:    // deme fitness = average organism life fitness at the current update
    for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        cDoubleSum single_deme_life_fitness;
	const cDeme & cur_deme = deme_array[deme_id];
        for (int i = 0; i < cur_deme.GetSize(); i++) {
          int cur_cell = cur_deme.GetCellID(i);
          if (cell_array[cur_cell].IsOccupied() == false) continue;
          cPhenotype & phenotype = GetCell(cur_cell).GetOrganism()->GetPhenotype();
          single_deme_life_fitness.Add(phenotype.GetLifeFitness());
        }
        deme_fitness[deme_id] = single_deme_life_fitness.Ave();
        total_fitness += deme_fitness[deme_id];
    }
    break; 
  case 6:     // deme fitness = 2^(-deme life fitness rank) (same as 4, but with life fitness)
    // first find all the deme fitness values ...
    {
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        cDoubleSum single_deme_life_fitness;
	const cDeme & cur_deme = deme_array[deme_id];
        for (int i = 0; i < cur_deme.GetSize(); i++) {
          int cur_cell = cur_deme.GetCellID(i);
          if (cell_array[cur_cell].IsOccupied() == false) continue;
          cPhenotype & phenotype = GetCell(cur_cell).GetOrganism()->GetPhenotype();
          single_deme_life_fitness.Add(phenotype.GetLifeFitness());
        }
        deme_fitness[deme_id] = single_deme_life_fitness.Ave();
      }
      // ... then determine the rank of each deme based on its fitness
      tArray<double> deme_rank(num_demes);
      deme_rank.SetAll(1);
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        for (int test_deme = 0; test_deme < num_demes; test_deme++) {
          if (deme_fitness[deme_id] < deme_fitness[test_deme]) {
            deme_rank[deme_id]++;
          }
        }
      }
      // ... finally, make deme fitness 2^(-deme rank)
      deme_fitness.SetAll(1);
      for (int deme_id = 0; deme_id < num_demes; deme_id++) {
        for (int i = 0; i < deme_rank[deme_id]; i++) {
          deme_fitness[deme_id] = deme_fitness[deme_id]/2;
        }
        total_fitness += deme_fitness[deme_id];
      }
    }
    break;
  } 
  
  // Pick which demes should be in the next generation.
  tArray<int> new_demes(num_demes);
  for (int i = 0; i < num_demes; i++) {
    double birth_choice = (double) m_world->GetRandom().GetDouble(total_fitness);
    double test_total = 0;
    for (int test_deme = 0; test_deme < num_demes; test_deme++) {
      test_total += deme_fitness[test_deme];
      if (birth_choice < test_total) {
        new_demes[i] = test_deme;
        break;
      }
    }
  }
  
  // Track how many of each deme we should have.
  tArray<int> deme_count(num_demes);
  deme_count.SetAll(0);
  for (int i = 0; i < num_demes; i++) {
    deme_count[new_demes[i]]++;
  }
  
  tArray<bool> is_init(num_demes); 
  is_init.SetAll(false);
  
  // Copy demes until all deme counts are 1.
  while (true) {
    // Find the next deme to copy...
    int from_deme_id, to_deme_id;
    for (from_deme_id = 0; from_deme_id < num_demes; from_deme_id++) {
      if (deme_count[from_deme_id] > 1) break;
    }
    
    // Stop If we didn't find another deme to copy
    if (from_deme_id == num_demes) break;
    
    for (to_deme_id = 0; to_deme_id < num_demes; to_deme_id++) {
      if (deme_count[to_deme_id] == 0) break;
    }
    
    // We now have both a from and a to deme....
    deme_count[from_deme_id]--;
    deme_count[to_deme_id]++;
    
    cDeme & from_deme = deme_array[from_deme_id];
    cDeme & to_deme   = deme_array[to_deme_id];

    // Do the actual copy!
    for (int i = 0; i < from_deme.GetSize(); i++) {
      int from_cell_id = from_deme.GetCellID(i);
      int to_cell_id = to_deme.GetCellID(i);
      if (cell_array[from_cell_id].IsOccupied() == true) {
        InjectClone( to_cell_id, *(cell_array[from_cell_id].GetOrganism()) );
      }
    }
    is_init[to_deme_id] = true;
  }
  
  // Now re-inject all remaining demes into themselves to reset them.
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    if (is_init[deme_id] == true) continue;
    cDeme & cur_deme = deme_array[deme_id];

    for (int i = 0; i < cur_deme.GetSize(); i++) {
      int cur_cell_id = cur_deme.GetCellID(i);
      if (cell_array[cur_cell_id].IsOccupied() == false) continue;
      InjectClone( cur_cell_id, *(cell_array[cur_cell_id].GetOrganism()) );
    }
  }
  
  // Reset all deme stats to zero.
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    deme_array[deme_id].Reset();
  }
}


/* Check if any demes have met the critera to be replicated and do so.
   There are several bases this can be checked on:

    0: 'all'       - ...all non-empty demes in the population.
    1: 'full_deme' - ...demes that have been filled up.
    2: 'corners'   - ...demes with upper left and lower right corners filled.
*/

void cPopulation::ReplicateDemes(int rep_trigger)
{
  // Determine which demes should be replicated.
  const int num_demes = GetNumDemes();
  cRandom & random = m_world->GetRandom();

  // Loop through all candidate demes...
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    cDeme & source_deme = deme_array[deme_id];

    // Test this deme to determine if it should be replicated.  If not,
    // continue on to the next deme.
    switch (rep_trigger) {
    case 0:    // CASE: Replicate all non-empty demes...
      // If this deme is empt, continue looping...
      if (source_deme.IsEmpty()) continue;
      break;
    case 1:    // Replicate all full demes...
      if (source_deme.IsFull() == false) continue;
      break;
    case 2:    // Replicate all demes with the corners filled in.
      {
	// The first and last IDs represent the two corners.
	const int id1 = source_deme.GetCellID(0);
	const int id2 = source_deme.GetCellID(source_deme.GetSize() - 1);
	if (cell_array[id1].IsOccupied() == false ||
	    cell_array[id2].IsOccupied() == false) continue;
      }
      break;
    default:
      cerr << "ERROR: Invalid replication trigger " << rep_trigger
	   << " in cPopulation::ReplicateDemes()" << endl;
      continue;
    }

    // -- If we made it this far, we should replicate this deme --

    // Choose a random organism from this deme...
    int cell1_id = -1;
    const int deme1_size = source_deme.GetSize();
    while (cell1_id == -1 || cell_array[cell1_id].IsOccupied() == false) {
      cell1_id = source_deme.GetCellID(random.GetUInt(deme1_size));
    }

    // Choose a random target deme to replicate to...
    int target_id = deme_id;
    while (target_id == deme_id) target_id = random.GetUInt(num_demes);
    cDeme & target_deme = deme_array[target_id];

    // Clear out existing cells in target deme.
    const int deme2_size = target_deme.GetSize();
    for (int i = 0; i < deme2_size; i++) {
      KillOrganism(cell_array[ target_deme.GetCellID(i) ]);
    }
    
    // And do the replication into the central cell of the target deme...
    const int cell2_id = target_deme.GetCellID( deme2_size/2 );
    InjectClone( cell2_id, *(cell_array[cell1_id].GetOrganism()) );    

    // Clear out the source deme to reset it
    for (int i = 0; i < deme1_size; i++) {
      KillOrganism(cell_array[ source_deme.GetCellID(i) ]);
    }

    // Inject the target offspring back into the source ID.
    const int cell3_id = source_deme.GetCellID( deme1_size/2 );
    InjectClone( cell3_id, *(cell_array[cell2_id].GetOrganism()) );        

    // Rotate both injected cells to face northwest.
    cell_array[cell2_id].Rotate(
		cell_array[GridNeighbor(cell2_id, world_x, world_y, -1, -1)] );
    cell_array[cell3_id].Rotate(
		cell_array[GridNeighbor(cell3_id, world_x, world_y, -1, -1)] );
  }
}


// Loop through all demes to determine if any are ready to be divided.  All
// full demes have 1/2 of their organisms (the odd ones) moved into a new deme.

void cPopulation::DivideDemes()
{
  // Determine which demes should be replicated.
  const int num_demes = GetNumDemes();
  cRandom & random = m_world->GetRandom();

  // Loop through all candidate demes...
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    cDeme & source_deme = deme_array[deme_id];

    // Only divide full demes.
    if (source_deme.IsFull() == false) continue;

    // Choose a random target deme to replicate to...
    int target_id = deme_id;
    while (target_id == deme_id) target_id = random.GetUInt(num_demes);
    cDeme & target_deme = deme_array[target_id];
    const int deme_size = target_deme.GetSize();

    // Clear out existing cells in target deme.
    for (int i = 0; i < deme_size; i++) {
      KillOrganism(cell_array[ target_deme.GetCellID(i) ]);
    }

    // Setup an array to collect the total number of tasks performed.
    const int num_tasks = cell_array[source_deme.GetCellID(0)].GetOrganism()->
      GetPhenotype().GetLastTaskCount().GetSize();
    tArray<int> tot_tasks(num_tasks);
    tot_tasks.SetAll(0);
    
    // Move over the odd numbered cells.
    for (int pos = 0; pos < deme_size; pos += 2) {
      const int cell1_id = source_deme.GetCellID( pos+1 );
      const int cell2_id = target_deme.GetCellID( pos );
      cOrganism * org1 = cell_array[cell1_id].GetOrganism();

      // Keep track of what tasks have been done.
      const tArray<int> & cur_tasks = org1->GetPhenotype().GetLastTaskCount();
      for (int i = 0; i < num_tasks; i++) {
	tot_tasks[i] += cur_tasks[i];
      }

      // Inject a copy of the odd organisms into the even cells.
      InjectClone( cell2_id, *org1 );    

      // Kill the organisms in the odd cells.
      KillOrganism( cell_array[cell1_id] );
    }
    
    // Figure out the merit each organism should have.
    int merit = 100;
    for (int i = 0; i < num_tasks; i++) {
      if (tot_tasks[i] > 0) merit *= 2;
    }

    // Setup the merit of both old and new individuals.
    for (int pos = 0; pos < deme_size; pos += 2) {
      cell_array[source_deme.GetCellID(pos)].GetOrganism()->UpdateMerit(merit);
      cell_array[target_deme.GetCellID(pos)].GetOrganism()->UpdateMerit(merit);
    }

  }
}


// Reset Demes goes through each deme and resets the individual organisms as
// if they were just injected into the population.

void cPopulation::ResetDemes()
{
  // re-inject all demes into themselves to reset them.
  for (int deme_id = 0; deme_id < deme_array.GetSize(); deme_id++) {
    for (int i = 0; i < deme_array[deme_id].GetSize(); i++) {
      int cur_cell_id = deme_array[deme_id].GetCellID(i);
      if (cell_array[cur_cell_id].IsOccupied() == false) continue;
      InjectClone( cur_cell_id, *(cell_array[cur_cell_id].GetOrganism()) );
    }
  }
}


// Copy the full contents of one deme into another.

void cPopulation::CopyDeme(int deme1_id, int deme2_id)
{
  cDeme & deme1 = deme_array[deme1_id];
  cDeme & deme2 = deme_array[deme2_id];

  for (int i = 0; i < deme1.GetSize(); i++) {
    int from_cell = deme1.GetCellID(i);
    int to_cell = deme2.GetCellID(i);
    if (cell_array[from_cell].IsOccupied() == false) {
      KillOrganism(cell_array[to_cell]);
      continue;
    }
    InjectClone( to_cell, *(cell_array[from_cell].GetOrganism()) );    
  }
}


// Copy a single indvidual out of a deme into a new one (which is first purged
// of existing organisms.)

void cPopulation::SpawnDeme(int deme1_id, int deme2_id)
{
  // Must spawn into a different deme.
  assert(deme1_id != deme2_id);

  const int num_demes = deme_array.GetSize();

  // If the second argument is a -1, choose a deme at random.
  cRandom & random = m_world->GetRandom();
  while (deme2_id == -1 || deme2_id == deme1_id) {
    deme2_id = random.GetUInt(num_demes);
  }

  // Make sure we have all legal values...
  assert(deme1_id >= 0 && deme1_id < num_demes);
  assert(deme2_id >= 0 && deme2_id < num_demes);

  // Find the demes that we're working with.
  cDeme & deme1 = deme_array[deme1_id];
  cDeme & deme2 = deme_array[deme2_id];

  // Make sure that the deme we're copying from has at least 1 organism.
  assert(deme1.GetOrgCount() > 0);

  // Determine the cell to copy from.
  int cell1_id = deme1.GetCellID( random.GetUInt(deme1.GetSize()) );
  while (cell_array[cell1_id].IsOccupied() == false) {
    cell1_id = deme1.GetCellID( random.GetUInt(deme1.GetSize()) );
  }
  
  // Clear out existing cells in target deme.
  for (int i = 0; i < deme2.GetSize(); i++) {
    KillOrganism(cell_array[ deme2.GetCellID(i) ]);
  }

  // And do the spawning.
  int cell2_id = deme2.GetCellID( random.GetUInt(deme2.GetSize()) );
  InjectClone( cell2_id, *(cell_array[cell1_id].GetOrganism()) );    
}


// Print out statistics about individual demes

void cPopulation::PrintDemeStats()
{
  cStats& stats = m_world->GetStats();
  
  cDataFile & df_fit = m_world->GetDataFile("deme_fitness.dat");
  cDataFile & df_life_fit = m_world->GetDataFile("deme_lifetime_fitness.dat");
  cDataFile & df_merit = m_world->GetDataFile("deme_merit.dat");
  cDataFile & df_gest = m_world->GetDataFile("deme_gest_time.dat");
  cDataFile & df_task = m_world->GetDataFile("deme_task.dat");
  cDataFile & df_donor = m_world->GetDataFile("deme_donor.dat");
  cDataFile & df_receiver = m_world->GetDataFile("deme_receiver.dat");
  
  df_fit.WriteComment("Average fitnesses for each deme in the population");
  df_life_fit.WriteComment("Average life fitnesses for each deme in the population");
  df_merit.WriteComment("Average merits for each deme in population");
  df_gest.WriteComment("Average gestation time for each deme in population");
  df_task.WriteComment("Num orgs doing each task for each deme in population");
  df_donor.WriteComment("Num orgs doing doing a donate for each deme in population");
  df_receiver.WriteComment("Num orgs doing receiving a donate for each deme in population");
  
  df_fit.WriteTimeStamp();
  df_life_fit.WriteTimeStamp();
  df_merit.WriteTimeStamp();
  df_gest.WriteTimeStamp();
  df_task.WriteTimeStamp();
  df_donor.WriteTimeStamp();
  df_receiver.WriteTimeStamp();
  
  df_fit.Write(stats.GetUpdate(), "update");
  df_life_fit.Write(stats.GetUpdate(), "update");
  df_merit.Write(stats.GetUpdate(), "update");
  df_gest.Write(stats.GetUpdate(), "update");
  df_task.Write(stats.GetUpdate(), "update");
  df_donor.Write(stats.GetUpdate(), "update");
  df_receiver.Write(stats.GetUpdate(), "update");
  
  const int num_inst = m_world->GetNumInstructions();
  const int num_task = environment.GetTaskLib().GetSize();
  
  const int num_demes = deme_array.GetSize();
  for (int deme_id = 0; deme_id < num_demes; deme_id++) {
    cString filename;
    filename.Set("deme_instruction-%d.dat", deme_id);
    cDataFile & df_inst = m_world->GetDataFile(filename); 
    cString comment;
    comment.Set("Number of times each instruction is exectued in deme %d",
                deme_id);
    df_inst.WriteComment(comment);
    df_inst.WriteTimeStamp();
    df_inst.Write(stats.GetUpdate(), "update");
    
    cDoubleSum single_deme_fitness;
    cDoubleSum single_deme_life_fitness;
    cDoubleSum single_deme_merit;
    cDoubleSum single_deme_gest_time;
    cDoubleSum single_deme_donor;
    cDoubleSum single_deme_receiver;
    tArray<cIntSum> single_deme_task(num_task);
    tArray<cIntSum> single_deme_inst(num_inst);
    
    const cDeme & cur_deme = deme_array[deme_id];
    for (int i = 0; i < cur_deme.GetSize(); i++) {
      int cur_cell = cur_deme.GetCellID(i);
      if (cell_array[cur_cell].IsOccupied() == false) continue;
      cPhenotype & phenotype = GetCell(cur_cell).GetOrganism()->GetPhenotype();
      single_deme_fitness.Add(phenotype.GetFitness()); 	
      single_deme_life_fitness.Add(phenotype.GetLifeFitness()); 	
      single_deme_merit.Add(phenotype.GetMerit().GetDouble()); 	
      single_deme_gest_time.Add(phenotype.GetGestationTime()); 	
      single_deme_donor.Add(phenotype.IsDonorLast()); 	
      single_deme_receiver.Add(phenotype.IsReceiver()); 	
      
      for (int j = 0; j < num_inst; j++) {
        single_deme_inst[j].Add(phenotype.GetLastInstCount()[j]);
      } 
      
      for (int j = 0; j < num_task; j++) {
        // only interested in tasks is done once! 
        if (phenotype.GetLastTaskCount()[j] > 0) {
          single_deme_task[j].Add(1);
        }
      }
    }
    
    comment.Set("Deme %d", deme_id);
    df_fit.Write(single_deme_fitness.Ave(), comment);
    df_life_fit.Write(single_deme_life_fitness.Ave(), comment);
    df_merit.Write(single_deme_merit.Ave(), comment);
    df_gest.Write(single_deme_gest_time.Ave(), comment);
    df_donor.Write(single_deme_donor.Sum(), comment);
    df_receiver.Write(single_deme_receiver.Sum(), comment);
    
    for (int j = 0; j < num_task; j++) {
      comment.Set("Deme %d, Task %d", deme_id, j);
      df_task.Write((int) single_deme_task[j].Sum(), comment);
    }
    
    for (int j = 0; j < num_inst; j++) {
      comment.Set("Inst %d", j);
      df_inst.Write((int) single_deme_inst[j].Sum(), comment);
    }
    df_inst.Endl();
  } 
  
  df_fit.Endl();
  df_life_fit.Endl();
  df_merit.Endl();
  df_gest.Endl();
  df_task.Endl();
  df_donor.Endl();
  df_receiver.Endl();
}


/**
* This function is responsible for adding an organism to a given lineage,
 * and setting the organism's lineage label and the lineage pointer.
 **/

void cPopulation::LineageSetupOrganism(cOrganism* organism, cLineage* lin, int lin_label, cGenotype* parent_genotype)
{
  // If we have some kind of lineage control, adjust the default values passed in.
  if (m_world->GetConfig().LOG_LINEAGES.Get()){
    lin = m_world->GetClassificationManager().GetLineage(m_world->GetDefaultContext(), organism->GetGenotype(), parent_genotype, lin, lin_label);
    lin_label = lin->GetID();
  }
  
  organism->SetLineageLabel( lin_label );
  organism->SetLineage( lin );
}


/**
* This function directs which position function should be used.  It
 * could have also been done with a function pointer, but the dividing
 * of an organism takes enough time that this will be a negligible addition,
 * and it gives a centralized function to work with.  The parent_ok flag asks
 * if it is okay to replace the parent.
 **/

cPopulationCell& cPopulation::PositionChild(cPopulationCell& parent_cell, bool parent_ok)
{
  assert(parent_cell.IsOccupied());
  
  const int birth_method = m_world->GetConfig().BIRTH_METHOD.Get();
  
  // Try out global/full-deme birth methods first...
  
  if (birth_method == POSITION_CHILD_FULL_SOUP_RANDOM) {
    int out_pos = m_world->GetRandom().GetUInt(cell_array.GetSize());
    while (parent_ok == false && out_pos == parent_cell.GetID()) {
      out_pos = m_world->GetRandom().GetUInt(cell_array.GetSize());
    }
    return GetCell(out_pos);
  }
  else if (birth_method == POSITION_CHILD_FULL_SOUP_ELDEST) {
    cPopulationCell * out_cell = reaper_queue.PopRear();
    if (parent_ok == false && out_cell->GetID() == parent_cell.GetID()) {
      out_cell = reaper_queue.PopRear();
      reaper_queue.PushRear(&parent_cell);
    }
    return *out_cell;
  }
  else if (birth_method == POSITION_CHILD_DEME_RANDOM) {
    const int deme_id = parent_cell.GetDemeID();
    const int deme_size = deme_array[deme_id].GetSize();

    int out_pos = m_world->GetRandom().GetUInt(deme_size);
    int out_cell_id = deme_array[deme_id].GetCellID(out_pos);
    while (parent_ok == false && out_cell_id == parent_cell.GetID()) {
      out_pos = m_world->GetRandom().GetUInt(deme_size);
      out_cell_id = deme_array[deme_id].GetCellID(out_pos);
    }

    deme_array[deme_id].IncBirthCount();
    return GetCell(out_cell_id);    
  }
  else if (birth_method == POSITION_CHILD_PARENT_FACING) {
    return parent_cell.GetCellFaced();
  }
  else if (birth_method == POSITION_CHILD_NEXT_CELL) {
    int out_cell_id = parent_cell.GetID() + 1;
    if (out_cell_id == cell_array.GetSize()) out_cell_id = 0;
    return GetCell(out_cell_id);
  }

  // All remaining methods require us to choose among mulitple local positions.

  // Construct a list of equally viable locations to place the child...
  tList<cPopulationCell> found_list;
  
  // First, check if there is an empty organism to work with (always preferred)
  tList<cPopulationCell> & conn_list = parent_cell.ConnectionList();
  
  if (m_world->GetConfig().PREFER_EMPTY.Get() == false &&
      birth_method == POSITION_CHILD_RANDOM) {
    found_list.Append(conn_list);
    if (parent_ok == true) found_list.Push(&parent_cell);
  } else {
    FindEmptyCell(conn_list, found_list);
  }
  
  // If we have not found an empty organism, we must use the specified function
  // to determine how to choose among the filled organisms.
  if (found_list.GetSize() == 0) {
    switch(birth_method) {
      case POSITION_CHILD_AGE:
        PositionAge(parent_cell, found_list, parent_ok);
        break;
      case POSITION_CHILD_MERIT:
        PositionMerit(parent_cell, found_list, parent_ok);
        break;
      case POSITION_CHILD_RANDOM:
        found_list.Append(conn_list);
        if (parent_ok == true) found_list.Push(&parent_cell);
          break;
      case POSITION_CHILD_EMPTY:
        // Nothing is in list if no empty cells are found...
        break;
    }
  }
  
  if (deme_array.GetSize() > 0) {
    const int deme_id = parent_cell.GetDemeID();
    deme_array[deme_id].IncBirthCount();
  }
  
  // If there are no possibilities, return parent.
  if (found_list.GetSize() == 0) return parent_cell;
  
  // Choose the organism randomly from those in the list, and return it.
  int choice = m_world->GetRandom().GetUInt(found_list.GetSize());
  return *( found_list.GetPos(choice) );
}


int cPopulation::ScheduleOrganism()
{
  return schedule->GetNextID();
}

void cPopulation::ProcessStep(cAvidaContext& ctx, double step_size, int cell_id)
{
  assert(step_size > 0.0);
  assert(cell_id < cell_array.GetSize());
  
  // If cell_id is negative, no cell could be found -- stop here.
  if (cell_id < 0) return;
  
  cPopulationCell& cell = GetCell(cell_id);
  assert(cell.IsOccupied()); // Unoccupied cell getting processor time!
  
  cOrganism* cur_org = cell.GetOrganism();
  cur_org->GetHardware().SingleProcess(ctx);
  if (cur_org->GetPhenotype().GetToDelete() == true) {
    delete cur_org;
  }
  m_world->GetStats().IncExecuted();
  resource_count.Update(step_size);
}


void cPopulation::UpdateOrganismStats()
{
  // Loop through all the cells getting stats and doing calculations
  // which must be done on a creature by creature basis.
  
  cStats& stats = m_world->GetStats();
  
  // Clear out organism sums...
  stats.SumFitness().Clear();
  stats.SumGestation().Clear();
  stats.SumMerit().Clear();
  stats.SumCreatureAge().Clear();
  stats.SumGeneration().Clear();
  stats.SumNeutralMetric().Clear();
  stats.SumLineageLabel().Clear();
  stats.SumCopyMutRate().Clear();
  stats.SumDivMutRate().Clear();
  stats.SumCopySize().Clear();
  stats.SumExeSize().Clear();
  stats.SumMemSize().Clear();
  
  
  stats.ZeroTasks();
  
#if INSTRUCTION_COUNT
  stats.ZeroInst();
#endif
  
  // Counts...
  int num_breed_true = 0;
  int num_parasites = 0;
  int num_no_birth = 0;
  int num_multi_thread = 0;
  int num_single_thread = 0;
  int num_modified = 0;
  
  // Maximums...
  cMerit max_merit(0);
  double max_fitness = 0;
  int max_gestation_time = 0;
  int max_genome_length = 0;
  
  // Minimums...
  cMerit min_merit(FLT_MAX);
  double min_fitness = FLT_MAX;
  int min_gestation_time = INT_MAX;
  int min_genome_length = INT_MAX;
  
  for (int i = 0; i < cell_array.GetSize(); i++) {
    // Only look at cells with organisms in them.
    if (cell_array[i].IsOccupied() == false) {
      
      // Genotype map needs zero for all non-occupied cells
      
      stats.SetGenoMapElement(i, 0);
      continue;
    }
    
    cOrganism * organism = cell_array[i].GetOrganism();
    const cPhenotype & phenotype = organism->GetPhenotype();
    const cMerit cur_merit = phenotype.GetMerit();
    const double cur_fitness = phenotype.GetFitness();
    const int cur_gestation_time = phenotype.GetGestationTime();
    const int cur_genome_length = phenotype.GetGenomeLength();
    
    stats.SumFitness().Add(cur_fitness);
    stats.SumMerit().Add(cur_merit.GetDouble());
    stats.SumGestation().Add(phenotype.GetGestationTime());
    stats.SumCreatureAge().Add(phenotype.GetAge());
    stats.SumGeneration().Add(phenotype.GetGeneration());
    stats.SumNeutralMetric().Add(phenotype.GetNeutralMetric());
    stats.SumLineageLabel().Add(organism->GetLineageLabel());
    stats.SumCopyMutRate().Add(organism->MutationRates().GetCopyMutProb());
    stats.SumLogCopyMutRate().Add(log(organism->MutationRates().GetCopyMutProb()));
    stats.SumDivMutRate().Add(organism->MutationRates().GetDivMutProb() / organism->GetPhenotype().GetDivType());
    stats.SumLogDivMutRate().Add(log(organism->MutationRates().GetDivMutProb() /organism->GetPhenotype().GetDivType()));
    stats.SumCopySize().Add(phenotype.GetCopiedSize());
    stats.SumExeSize().Add(phenotype.GetExecutedSize());
    stats.SetGenoMapElement(i, organism->GetGenotype()->GetID());
    
#if INSTRUCTION_COUNT
    for (int j=0; j < m_world->GetNumInstructions(); j++) {
      stats.SumExeInst()[j].Add(organism->GetPhenotype().GetLastInstCount()[j]);
    }
#endif
    
    if (cur_merit > max_merit) max_merit = cur_merit;
    if (cur_fitness > max_fitness) max_fitness = cur_fitness;
    if (cur_gestation_time > max_gestation_time) max_gestation_time = cur_gestation_time;
    if (cur_genome_length > max_genome_length) max_genome_length = cur_genome_length;
    
    if (cur_merit < min_merit) min_merit = cur_merit;
    if (cur_fitness < min_fitness) min_fitness = cur_fitness;
    if (cur_gestation_time < min_gestation_time) min_gestation_time = cur_gestation_time;
    if (cur_genome_length < min_genome_length) min_genome_length = cur_genome_length;
    
    // Test what tasks this creatures has completed.
    for (int j=0; j < m_world->GetEnvironment().GetTaskLib().GetSize(); j++) {
      if (phenotype.GetCurTaskCount()[j] > 0)
	  {
		  stats.AddCurTask(j);
		  stats.AddCurTaskQuality(j, phenotype.GetCurTaskQuality()[j]);
	  }
      if (phenotype.GetLastTaskCount()[j] > 0)
	  {
		  stats.AddLastTask(j);
		  stats.AddLastTaskQuality(j, phenotype.GetLastTaskQuality()[j]);
		  stats.IncTaskExeCount(j, phenotype.GetLastTaskCount()[j]);
	  } 
    }
    
    // Test what resource combinations this creature has sensed
    for (int j=0; j < stats.GetSenseSize(); j++) {
      if (phenotype.GetLastSenseCount()[j] > 0)
	  {
		  stats.AddLastSense(j);
		  stats.IncLastSenseExeCount(j, phenotype.GetLastSenseCount()[j]);
	  }
    }
    
    // Increment the counts for all qualities the organism has...
    if (phenotype.ParentTrue()) num_breed_true++;
    if (phenotype.IsParasite()) num_parasites++;
    if( phenotype.GetNumDivides() == 0 ) num_no_birth++;
    if(phenotype.IsMultiThread()) num_multi_thread++;
    else num_single_thread++;
    if(phenotype.IsModified()) num_modified++;    
    
    // Hardware specific collections...
    if (organism->GetHardware().GetType() == HARDWARE_TYPE_CPU_ORIGINAL) {
      cHardwareBase & hardware = organism->GetHardware();
      stats.SumMemSize().Add(hardware.GetMemory().GetSize());
    }
    
    // Increment the age of this organism.
    organism->GetPhenotype().IncAge();
    }
  
  stats.SetBreedTrueCreatures(num_breed_true);
  stats.SetNumNoBirthCreatures(num_no_birth);
  stats.SetNumParasites(num_parasites);
  stats.SetNumSingleThreadCreatures(num_single_thread);
  stats.SetNumMultiThreadCreatures(num_multi_thread);
  stats.SetNumModified(num_modified);
  
  stats.SetMaxMerit(max_merit.GetDouble());
  stats.SetMaxFitness(max_fitness);
  stats.SetMaxGestationTime(max_gestation_time);
  stats.SetMaxGenomeLength(max_genome_length);
  
  stats.SetMinMerit(min_merit.GetDouble());
  stats.SetMinFitness(min_fitness);
  stats.SetMinGestationTime(min_gestation_time);
  stats.SetMinGenomeLength(min_genome_length);
  
  stats.SetResources(resource_count.GetResources());
  stats.SetSpatialRes(resource_count.GetSpatialRes());
  stats.SetResourcesGeometry(resource_count.GetResourcesGeometry());
  }


void cPopulation::UpdateGenotypeStats()
{
  // Loop through all genotypes, finding stats and doing calcuations.
  
  cStats& stats = m_world->GetStats();
  
  // Clear out genotype sums...
  stats.SumGenotypeAge().Clear();
  stats.SumAbundance().Clear();
  stats.SumGenotypeDepth().Clear();
  stats.SumSize().Clear();
  stats.SumThresholdAge().Clear();
  
  double entropy = 0.0;
  
  cGenotype * cur_genotype = m_world->GetClassificationManager().GetBestGenotype();
  for (int i = 0; i < m_world->GetClassificationManager().GetGenotypeCount(); i++) {
    const int abundance = cur_genotype->GetNumOrganisms();
    
    // If we're at a dead genotype, we've hit the end of the list!
    if (abundance == 0) break;
    
    // Update stats...
    const int age = stats.GetUpdate() - cur_genotype->GetUpdateBorn();
    stats.SumGenotypeAge().Add(age, abundance);
    stats.SumAbundance().Add(abundance);
    stats.SumGenotypeDepth().Add(cur_genotype->GetDepth(), abundance);
    stats.SumSize().Add(cur_genotype->GetLength(), abundance);
    
    // Calculate this genotype's contribution to entropy
    const double p = ((double) abundance) / (double) num_organisms;
    const double partial_ent = -(p * Log(p));
    entropy += partial_ent;
    
    // Do any special calculations for threshold genotypes.
    if (cur_genotype->GetThreshold()) {
      stats.SumThresholdAge().Add(age, abundance);
    }
    
    // ...and advance to the next genotype...
    cur_genotype = cur_genotype->GetNext();
  }
  
  stats.SetEntropy(entropy);
}


void cPopulation::UpdateSpeciesStats()
{
  cStats& stats = m_world->GetStats();
  double species_entropy = 0.0;
  
  stats.SumSpeciesAge().Clear();
  
  // Loop through all species that need to be reset prior to calculations.
  cSpecies * cur_species = m_world->GetClassificationManager().GetFirstSpecies();
  for (int i = 0; i < m_world->GetClassificationManager().GetNumSpecies(); i++) {
    cur_species->ResetStats();
    cur_species = cur_species->GetNext();
  }
  
  // Collect info from genotypes and send it to their species.
  cGenotype * genotype = m_world->GetClassificationManager().GetBestGenotype();
  for (int i = 0; i < m_world->GetClassificationManager().GetGenotypeCount(); i++) {
    if (genotype->GetSpecies() != NULL) {
      genotype->GetSpecies()->AddOrganisms(genotype->GetNumOrganisms());
    }
    genotype = genotype->GetNext();
  }
  
  // Loop through all of the species in the soup, taking info on them.
  cur_species = m_world->GetClassificationManager().GetFirstSpecies();
  for (int i = 0; i < m_world->GetClassificationManager().GetNumSpecies(); i++) {
    const int abundance = cur_species->GetNumOrganisms();
    // const int num_genotypes = cur_species->GetNumGenotypes();
    
    // Basic statistical collection...
    const int species_age = stats.GetUpdate() - cur_species->GetUpdateBorn();
    stats.SumSpeciesAge().Add(species_age, abundance);
    
    // Caculate entropy on the species level...
    if (abundance > 0) {
      double p = ((double) abundance) / (double) num_organisms;
      double partial_ent = -(p * Log(p));
      species_entropy += partial_ent;
    }
    
    // ...and advance to the next species...
    cur_species = cur_species->GetNext();
  }
  
  stats.SetSpeciesEntropy(species_entropy);
}

void cPopulation::UpdateDominantStats()
{
  cStats& stats = m_world->GetStats();
  cGenotype * dom_genotype = m_world->GetClassificationManager().GetBestGenotype();
  if (dom_genotype == NULL) return;
  
  stats.SetDomGenotype(dom_genotype);
  stats.SetDomMerit(dom_genotype->GetMerit());
  stats.SetDomGestation(dom_genotype->GetGestationTime());
  stats.SetDomReproRate(dom_genotype->GetReproRate());
  stats.SetDomFitness(dom_genotype->GetFitness());
  stats.SetDomCopiedSize(dom_genotype->GetCopiedSize());
  stats.SetDomExeSize(dom_genotype->GetExecutedSize());
  
  stats.SetDomSize(dom_genotype->GetLength());
  stats.SetDomID(dom_genotype->GetID());
  stats.SetDomName(dom_genotype->GetName());
  stats.SetDomBirths(dom_genotype->GetThisBirths());
  stats.SetDomBreedTrue(dom_genotype->GetThisBreedTrue());
  stats.SetDomBreedIn(dom_genotype->GetThisBreedIn());
  stats.SetDomBreedOut(dom_genotype->GetThisBreedOut());
  stats.SetDomAbundance(dom_genotype->GetNumOrganisms());
  stats.SetDomGeneDepth(dom_genotype->GetDepth());
  stats.SetDomSequence(dom_genotype->GetGenome().AsString());
}

void cPopulation::UpdateDominantParaStats()
{
  cStats& stats = m_world->GetStats();
  cInjectGenotype * dom_inj_genotype = m_world->GetClassificationManager().GetBestInjectGenotype();
  if (dom_inj_genotype == NULL) return;
  
  stats.SetDomInjGenotype(dom_inj_genotype);
  
  stats.SetDomInjSize(dom_inj_genotype->GetLength());
  stats.SetDomInjID(dom_inj_genotype->GetID());
  stats.SetDomInjName(dom_inj_genotype->GetName());
  stats.SetDomInjAbundance(dom_inj_genotype->GetNumInjected());
  stats.SetDomInjSequence(dom_inj_genotype->GetGenome().AsString());
}

void cPopulation::CalcUpdateStats()
{
  cStats& stats = m_world->GetStats();
  // Reset the Genebank to prepare it for stat collection.
  m_world->GetClassificationManager().UpdateReset();
  
  UpdateOrganismStats();
  UpdateGenotypeStats();
  UpdateSpeciesStats();
  UpdateDominantStats();
  UpdateDominantParaStats();
  
  // Do any final calculations...
  stats.SetNumCreatures(GetNumOrganisms());
  stats.SetNumGenotypes(m_world->GetClassificationManager().GetGenotypeCount());
  stats.SetNumThreshSpecies(m_world->GetClassificationManager().GetNumSpecies());
  
  // Have stats calculate anything it now can...
  stats.CalcEnergy();
  stats.CalcFidelity();
}


bool cPopulation::SaveClone(ofstream& fp)
{
  if (fp.good() == false) return false;
  
  // Save the current update
  fp << m_world->GetStats().GetUpdate() << " ";
  
  // Save the archive info.
  m_world->GetClassificationManager().SaveClone(fp);
  
  // Save the genotypes manually.
  fp << m_world->GetClassificationManager().GetGenotypeCount() << " ";
  
  cGenotype * cur_genotype = m_world->GetClassificationManager().GetBestGenotype();
  for (int i = 0; i < m_world->GetClassificationManager().GetGenotypeCount(); i++) {
    cur_genotype->SaveClone(fp);
    
    // Advance...
    cur_genotype = cur_genotype->GetNext();
  }
  
  // Save the organim layout...
  fp << cell_array.GetSize() << " ";
  for (int i = 0; i < cell_array.GetSize(); i++) {
    if (cell_array[i].IsOccupied() == true) {
      fp <<  cell_array[i].GetOrganism()->GetGenotype()->GetID() << " ";
    }
    else fp << "-1 ";
  }
  
  return true;
}


bool cPopulation::LoadClone(ifstream & fp)
{
  if (fp.good() == false) return false;
  
  // Pick up the update where it was left off.
  int cur_update;
  fp >> cur_update;
  
  m_world->GetStats().SetCurrentUpdate(cur_update);
  
  // Clear out the population
  for (int i = 0; i < cell_array.GetSize(); i++) KillOrganism(cell_array[i]);
  
  // Load the archive info.
  m_world->GetClassificationManager().LoadClone(fp);
  
  // Load up the genotypes.
  int num_genotypes = 0;
  fp >> num_genotypes;
  
  cGenotype** genotype_array = new cGenotype*[num_genotypes];
  for (int i = 0; i < num_genotypes; i++) {
    genotype_array[i] = cGenotype::LoadClone(m_world, fp);
  }
  
  // Now load them into the organims.  @CAO make sure cell_array.GetSize() is right!
  int in_num_cells;
  int genotype_id;
  fp >> in_num_cells;
  if (cell_array.GetSize() != in_num_cells) return false;
  
  for (int i = 0; i < cell_array.GetSize(); i++) {
    fp >> genotype_id;
    if (genotype_id == -1) continue;
    int genotype_index = -1;
    for (int j = 0; j < num_genotypes; j++) {
      if (genotype_array[j]->GetID() == genotype_id) {
        genotype_index = j;
        break;
      }
    }
    
    assert(genotype_index != -1);
    InjectGenome(i, genotype_array[genotype_index]->GetGenome(), 0);
  }
  
  sync_events = true;
  
  return true;
}

// This class is needed for the next function
class cTmpGenotype {
public:
  int id_num;
  int parent_id;
  int num_cpus;
  int total_cpus;
  double merit;
  int update_born;
  int update_dead;
  
  cGenotype *genotype;
  
  bool operator<( const cTmpGenotype rhs ) const {
    return id_num < rhs.id_num; }
};	


bool cPopulation::LoadDumpFile(cString filename, int update)
{
  // set the update if requested
  if ( update >= 0 )
    m_world->GetStats().SetCurrentUpdate(update);
  
  // Clear out the population
  for (int i = 0; i < cell_array.GetSize(); i++) KillOrganism(cell_array[i]);
  
  cout << "Loading: " << filename << endl;
  
  cInitFile input_file(filename);
  if (!input_file.IsOpen()) {
    cerr << "Error: Cannot load file: \"" << filename << "\"." << endl;
    exit(1);
  }
  input_file.Load();
  input_file.Compress();
  input_file.Close();
  
  // First, we read in all the genotypes and store them in a list
  
  vector<cTmpGenotype> genotype_vect;
  
  for (int line_id = 0; line_id < input_file.GetNumLines(); line_id++) {
    cString cur_line = input_file.GetLine(line_id);
    
    // Setup the genotype for this line...
    cTmpGenotype tmp;
    tmp.id_num      = cur_line.PopWord().AsInt();
    tmp.parent_id   = cur_line.PopWord().AsInt();
    /*parent_dist =*/          cur_line.PopWord().AsInt();
    tmp.num_cpus    = cur_line.PopWord().AsInt();
    tmp.total_cpus  = cur_line.PopWord().AsInt();
    /*length      =*/          cur_line.PopWord().AsInt();
    tmp.merit 	    = cur_line.PopWord().AsDouble();
    /*gest_time   =*/ cur_line.PopWord().AsInt();
    /*fitness     =*/ cur_line.PopWord().AsDouble();
    tmp.update_born = cur_line.PopWord().AsInt();
    tmp.update_dead = cur_line.PopWord().AsInt();
    /*depth       =*/ cur_line.PopWord().AsInt();
    cString name = cStringUtil::Stringf("org-%d", tmp.id_num);
    cGenome genome( cur_line.PopWord() );
    
    // we don't allow birth or death times larger than the current update
    if ( m_world->GetStats().GetUpdate() > tmp.update_born )
      tmp.update_born = m_world->GetStats().GetUpdate();
    if ( m_world->GetStats().GetUpdate() > tmp.update_dead )
      tmp.update_dead = m_world->GetStats().GetUpdate();
    
    tmp.genotype = m_world->GetClassificationManager().GetGenotypeLoaded(genome, tmp.update_born, tmp.id_num);
    tmp.genotype->SetName( name );
    
    genotype_vect.push_back( tmp );
  }
  
  // now, we sort them in ascending order according to their id_num
  sort( genotype_vect.begin(), genotype_vect.end() );
  // set the parents correctly
  
  vector<cTmpGenotype>::const_iterator it = genotype_vect.begin();
  for ( ; it != genotype_vect.end(); it++ ){
    vector<cTmpGenotype>::const_iterator it2 = it;
    cGenotype *parent = 0;
    // search backwards till we find the parent
    if ( it2 != genotype_vect.begin() )
      do{
        it2--;
        if ( (*it).parent_id == (*it2).id_num ){
          parent = (*it2).genotype;
          break;
        }	
      }
        while ( it2 != genotype_vect.begin() );
    (*it).genotype->SetParent( parent, NULL );
  }
  
  int cur_update = m_world->GetStats().GetUpdate(); 
  int current_cell = 0;
  bool soup_full = false;
  it = genotype_vect.begin();
  for ( ; it != genotype_vect.end(); it++ ){
    if ( (*it).num_cpus == 0 ){ // historic organism
                                // remove immediately, so that it gets transferred into the
                                // historic database. We change the update temporarily to the
                                // true death time of this organism, so that all stats are correct.
      m_world->GetStats().SetCurrentUpdate( (*it).update_dead );
      m_world->GetClassificationManager().RemoveGenotype( *(*it).genotype );
      m_world->GetStats().SetCurrentUpdate( cur_update );
    }
    else{ // otherwise, we insert as many organisms as we need
      for ( int i=0; i<(*it).num_cpus; i++ ){
        if ( current_cell >= cell_array.GetSize() ){
          soup_full = true;
          break;
        }	  
        InjectGenotype( current_cell, (*it).genotype );
        cPhenotype & phenotype = GetCell(current_cell).GetOrganism()->GetPhenotype();
        if ( (*it).merit > 0) phenotype.SetMerit( cMerit((*it).merit) );
        schedule->Adjust(current_cell, phenotype.GetMerit());
        
        int lineage_label = 0;
        LineageSetupOrganism(GetCell(current_cell).GetOrganism(),
                             0, lineage_label,
                             (*it).genotype->GetParentGenotype());
        current_cell += 1;
      }
    }
    cout << (*it).id_num << " "
      << (*it).parent_id << " "
      << (*it).genotype->GetParentID() << " "
      << (*it).genotype->GetNumOffspringGenotypes() << " "
      << (*it).num_cpus << " "
      << (*it).genotype->GetNumOrganisms() << endl;
    if (soup_full){
      cout << "cPopulation::LoadDumpFile: You are trying to load more organisms than there is space!" << endl;
      cout << "cPopulation::LoadDumpFile: Remaining organisms are ignored." << endl;
      break;
    }
  }
  sync_events = true;
  
  return true;
}


bool cPopulation::DumpMemorySummary(ofstream& fp)
{
  if (fp.good() == false) return false;
  
  // Dump the memory...
  
  for (int i = 0; i < cell_array.GetSize(); i++) {
    fp << i << " ";
    if (cell_array[i].IsOccupied() == false) {
      fp << "EMPTY" << endl;
    }
    else {
      cGenome & mem = cell_array[i].GetOrganism()->GetHardware().GetMemory();
      fp << mem.GetSize() << " "
        << mem.AsString() << endl;
    }
  }
  return true;
}

bool cPopulation::OK()
{
  // First check all sub-objects...
  if (!schedule->OK()) return false;
  
  // Next check organisms...
  for (int i = 0; i < cell_array.GetSize(); i++) {
    if (cell_array[i].OK() == false) return false;
    assert(cell_array[i].GetID() == i);
  }
  
  // And stats...
  assert(world_x * world_y == cell_array.GetSize());
  
  return true;
}


/**
* This function loads a genome from a given file, and initializes
 * a cpu with it.
 *
 * @param filename The name of the file to load.
 * @param in_cpu The grid-position into which the genome should be loaded.
 * @param merit An initial merit value.
 * @param lineage_label A value that allows to track the daughters of
 * this organism.
 **/

void cPopulation::Inject(const cGenome & genome, int cell_id, double merit, int lineage_label, double neutral)
{
  // If an invalid cell was given, choose a new ID for it.
  if (cell_id < 0) {
    switch (m_world->GetConfig().BIRTH_METHOD.Get()) {
      case POSITION_CHILD_FULL_SOUP_ELDEST:
        cell_id = reaper_queue.PopRear()->GetID();
      default:
        cell_id = 0;
    }
  }
  
  InjectGenome(cell_id, genome, lineage_label);
  cPhenotype& phenotype = GetCell(cell_id).GetOrganism()->GetPhenotype();
  phenotype.SetNeutralMetric(neutral);
  
  if (merit > 0) phenotype.SetMerit(cMerit(merit));
  schedule->Adjust(cell_id, phenotype.GetMerit());
  
  LineageSetupOrganism(GetCell(cell_id).GetOrganism(), 0, lineage_label);
}

void cPopulation::InjectParasite(const cCodeLabel& label, const cGenome& injected_code, int cell_id)
{
  cOrganism* target_organism = cell_array[cell_id].GetOrganism();
  
  if (target_organism == NULL) return;
  
  cHardwareBase& child_cpu = target_organism->GetHardware();
  if (child_cpu.GetNumThreads() == m_world->GetConfig().MAX_CPU_THREADS.Get()) return;
  
  if (target_organism->InjectHost(label, injected_code)) {
    cInjectGenotype* child_genotype = m_world->GetClassificationManager().GetInjectGenotype(injected_code, NULL);
    
    target_organism->AddParasite(child_genotype);
    child_genotype->AddParasite();
    child_cpu.ThreadSetOwner(child_genotype);
    m_world->GetClassificationManager().AdjustInjectGenotype(*child_genotype);
  }
}


cPopulationCell& cPopulation::GetCell(int in_num)
{
  return cell_array[in_num];
}


void cPopulation::UpdateResources(const tArray<double> & res_change)
{
  resource_count.Modify(res_change);
}

void cPopulation::UpdateResource(int id, double change)
{
  resource_count.Modify(id, change);
}

void cPopulation::UpdateCellResources(const tArray<double> & res_change, 
                                      const int cell_id)
{
  resource_count.ModifyCell(res_change, cell_id);
}

void cPopulation::SetResource(int id, double new_level)
{
  resource_count.Set(id, new_level);
}

void cPopulation::BuildTimeSlicer(cChangeList * change_list)
{
  switch (m_world->GetConfig().SLICING_METHOD.Get()) {
    case SLICE_CONSTANT:
      schedule = new cConstSchedule(cell_array.GetSize());
      break;
    case SLICE_PROB_MERIT:
      schedule = new cProbSchedule(cell_array.GetSize(), m_world->GetRandom().GetInt(0x7FFFFFFF));
      break;
    case SLICE_INTEGRATED_MERIT:
      schedule = new cIntegratedSchedule(cell_array.GetSize());
      break;
    default:
      cout << "Warning: Requested Time Slicer not found, defaulting to Integrated." << endl;
      schedule = new cIntegratedSchedule(cell_array.GetSize());
      break;
  }
  schedule->SetChangeList(change_list);
}


void cPopulation::PositionAge(cPopulationCell & parent_cell,
                              tList<cPopulationCell> & found_list,
                              bool parent_ok)
{
  // Start with the parent organism as the replacement, and see if we can find
  // anything equivilent or better.
  
  found_list.Push(&parent_cell);
  int max_age = parent_cell.GetOrganism()->GetPhenotype().GetAge();
  if (parent_ok == false) max_age = -1;
  
  // Now look at all of the neighbors.
  tListIterator<cPopulationCell> conn_it( parent_cell.ConnectionList() );
  
  cPopulationCell * test_cell;
  while ( (test_cell = conn_it.Next()) != NULL) {
    const int cur_age = test_cell->GetOrganism()->GetPhenotype().GetAge();
    if (cur_age > max_age) {
      max_age = cur_age;
      found_list.Clear();
      found_list.Push(test_cell);
    }
    else if (cur_age == max_age) {
      found_list.Push(test_cell);
    }
  }
}

void cPopulation::PositionMerit(cPopulationCell & parent_cell,
                                tList<cPopulationCell> & found_list,
                                bool parent_ok)
{
  // Start with the parent organism as the replacement, and see if we can find
  // anything equivilent or better.
  
  found_list.Push(&parent_cell);
  double max_ratio = parent_cell.GetOrganism()->CalcMeritRatio();
  if (parent_ok == false) max_ratio = -1;
  
  // Now look at all of the neighbors.
  tListIterator<cPopulationCell> conn_it( parent_cell.ConnectionList() );
  
  cPopulationCell * test_cell;
  while ( (test_cell = conn_it.Next()) != NULL) {
    const double cur_ratio = test_cell->GetOrganism()->CalcMeritRatio();
    if (cur_ratio > max_ratio) {
      max_ratio = cur_ratio;
      found_list.Clear();
      found_list.Push(test_cell);
    }
    else if (cur_ratio == max_ratio) {
      found_list.Push(test_cell);
    }
  }
}

void cPopulation::FindEmptyCell(tList<cPopulationCell> & cell_list,
                                tList<cPopulationCell> & found_list)
{
  tListIterator<cPopulationCell> cell_it(cell_list);
  cPopulationCell * test_cell;
  
  while ( (test_cell = cell_it.Next()) != NULL) {
    // If this cell is empty, add it to the list...
    if (test_cell->IsOccupied() == false) found_list.Push(test_cell);
  }
}

// This function injects a new organism into the population at cell_id based
// on the genotype passed in.
void cPopulation::InjectGenotype(int cell_id, cGenotype *new_genotype)
{
  assert(cell_id >= 0 && cell_id < cell_array.GetSize());
  
  cAvidaContext& ctx = m_world->GetDefaultContext();
  
  cOrganism* new_organism = new cOrganism(m_world, ctx, new_genotype->GetGenome());
  
  // Set the genotype...
  new_organism->SetGenotype(new_genotype);
  
  // Setup the phenotype...
  cPhenotype & phenotype = new_organism->GetPhenotype();
  phenotype.SetupInject(new_genotype->GetLength());
  phenotype.SetMerit( cMerit(new_genotype->GetTestMerit(ctx)) );
  
  // @CAO are these really needed?
  phenotype.SetLinesCopied( new_genotype->GetTestCopiedSize(ctx) );
  phenotype.SetLinesExecuted( new_genotype->GetTestExecutedSize(ctx) );
  phenotype.SetGestationTime( new_genotype->GetTestGestationTime(ctx) );
  
  // Prep the cell..
  if (m_world->GetConfig().BIRTH_METHOD.Get() == POSITION_CHILD_FULL_SOUP_ELDEST &&
      cell_array[cell_id].IsOccupied() == true) {
    // Have to manually take this cell out of the reaper Queue.
    reaper_queue.Remove( &(cell_array[cell_id]) );
  }
  
  // Setup the child's mutation rates.  Since this organism is being injected
  // and has no parent, we should always take the rate from the environment.
  new_organism->MutationRates().Copy(cell_array[cell_id].MutationRates());
  
  
  // Activate the organism in the population...
  ActivateOrganism(ctx, new_organism, cell_array[cell_id]);
}


// This function injects a new organism into the population at cell_id that
// is an exact clone of the organism passed in.

void cPopulation::InjectClone(int cell_id, cOrganism& orig_org)
{
  assert(cell_id >= 0 && cell_id < cell_array.GetSize());
  
  cAvidaContext& ctx = m_world->GetDefaultContext();
  
  cOrganism* new_organism = new cOrganism(m_world, ctx, orig_org.GetGenome());
  
  // Set the genotype...
  new_organism->SetGenotype(orig_org.GetGenotype());
  
  // Setup the phenotype...
  new_organism->GetPhenotype().SetupClone(orig_org.GetPhenotype());
  
  // Prep the cell..
  if (m_world->GetConfig().BIRTH_METHOD.Get() == POSITION_CHILD_FULL_SOUP_ELDEST &&
      cell_array[cell_id].IsOccupied() == true) {
    // Have to manually take this cell out of the reaper Queue.
    reaper_queue.Remove( &(cell_array[cell_id]) );
  }
  
  // Setup the mutation rate based on the population cell...
  new_organism->MutationRates().Copy(cell_array[cell_id].MutationRates());
  
  // Activate the organism in the population...
  ActivateOrganism(ctx, new_organism, cell_array[cell_id]);
}


void cPopulation::InjectGenome(int cell_id, const cGenome& genome, int lineage_label)
{
  // Setup the genotype...
  cGenotype* new_genotype = m_world->GetClassificationManager().GetGenotypeInjected(genome, lineage_label);
  
  // The rest is done by InjectGenotype();
  InjectGenotype( cell_id, new_genotype );
}


void cPopulation::SerialTransfer(int transfer_size, bool ignore_deads)
{
  assert(transfer_size > 0);
  
  // If we are ignoring all dead organisms, remove them from the population.
  if (ignore_deads == true) {
    for (int i = 0; i < GetSize(); i++) {
      cPopulationCell & cell = cell_array[i];
      if (cell.IsOccupied() && cell.GetOrganism()->GetTestFitness(m_world->GetDefaultContext()) == 0.0) {
        KillOrganism(cell);
      }
    }
  }
  
  // If removing the dead was enough, stop here.
  if (num_organisms <= transfer_size) return;
  
  // Collect a vector of the occupied cells...
  vector<int> transfer_pool;
  transfer_pool.reserve(num_organisms);
  for (int i = 0; i < GetSize(); i++) {
    if (cell_array[i].IsOccupied()) transfer_pool.push_back(i);
  }
  
  // Remove the proper number of cells.
  const int removal_size = num_organisms - transfer_size;
  for (int i = 0; i < removal_size; i++) {
    int j = (int) m_world->GetRandom().GetUInt(transfer_pool.size());
    KillOrganism(cell_array[transfer_pool[j]]);
    transfer_pool[j] = transfer_pool.back();
    transfer_pool.pop_back();
  }
}


void cPopulation::PrintPhenotypeData(const cString& filename)
{
  set<int> ids;

  for (int i = 0; i < cell_array.GetSize(); i++) {
    // Only look at cells with organisms in them.
    if (cell_array[i].IsOccupied() == false) continue;
    
    const cPhenotype& phenotype = cell_array[i].GetOrganism()->GetPhenotype();
    
    int id = 0;
    for (int j = 0; j < phenotype.GetLastTaskCount().GetSize(); j++) {
      if (phenotype.GetLastTaskCount()[j] > 0) id += (1 << j);
    }
    ids.insert(id);
  }
  
  cDataFile& df = m_world->GetDataFile(filename);
  df.WriteTimeStamp();
  df.Write(m_world->GetStats().GetUpdate(), "Update");
  df.Write(static_cast<int>(ids.size()), "Unique Phenotypes");
  df.Endl();
}

void cPopulation::PrintPhenotypeStatus(const cString& filename)
{
  cDataFile& df_phen = m_world->GetDataFile(filename);
  
  df_phen.WriteComment("Num orgs doing each task for each deme in population");
  df_phen.WriteTimeStamp();
  df_phen.Write(m_world->GetStats().GetUpdate(), "Update");
  
  cString comment;
  
  for (int i = 0; i < cell_array.GetSize(); i++) 
  {
    // Only look at cells with organisms in them.
    if (cell_array[i].IsOccupied() == false) continue;
    
    const cPhenotype& phenotype = cell_array[i].GetOrganism()->GetPhenotype();
    
    comment.Set("cur_merit %d;", i); 
    df_phen.Write(phenotype.GetMerit().GetDouble(), comment); 
    
    comment.Set("cur_merit_base %d;", i); 
    df_phen.Write(phenotype.GetCurMeritBase(), comment); 
    
    comment.Set("cur_merit_bonus %d;", i); 
    df_phen.Write(phenotype.GetCurBonus(), comment); 
    
    //    comment.Set("last_merit %d", i); 
    //    df_phen.Write(phenotype.GetLastMerit(), comment); 
    
    comment.Set("last_merit_base %d", i); 
    df_phen.Write(phenotype.GetLastMeritBase(), comment); 
    
    comment.Set("last_merit_bonus %d", i); 
    df_phen.Write(phenotype.GetLastBonus(), comment); 
    
    comment.Set("life_fitness %d", i); 
    df_phen.Write(phenotype.GetLifeFitness(), comment); 
    
    comment.Set("*"); 
    df_phen.Write("*", comment); 
    
  } 
  df_phen.Endl();
  
}     


bool cPopulation::UpdateMerit(int cell_id, double new_merit)
{
  assert( GetCell(cell_id).IsOccupied() == true);
  assert( new_merit >= 0.0 );
  
  cPhenotype & phenotype = GetCell(cell_id).GetOrganism()->GetPhenotype();
  double old_merit = phenotype.GetMerit().GetDouble(); 
  
  phenotype.SetMerit( cMerit(new_merit) );
  phenotype.SetLifeFitness(new_merit/phenotype.GetGestationTime()); 
  if (new_merit <= old_merit) {
	  phenotype.SetIsDonorCur(); }  
  else  { phenotype.SetIsReceiver(); } 
  
  schedule->Adjust(cell_id, phenotype.GetMerit());
  
  return true;
}

void cPopulation::SetChangeList(cChangeList *change_list){
  schedule->SetChangeList(change_list);
}
cChangeList *cPopulation::GetChangeList(){
  return schedule->GetChangeList();
}
