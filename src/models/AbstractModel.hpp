#ifndef ABSTRACTMODEL_HPP_
#define ABSTRACTMODEL_HPP_

#include <vector>
#include <string>
#include <map>
#include <memory>

#include "src/snapshotter/VTKSnapshotter.hpp"
#include "src/mesh/TriangleMesh.hpp"
//#include "util/utils.h"

#include "adolc/drivers/drivers.h"
#include "adolc/adolc.h"

template <typename TVarContainer, typename propsType, template <typename TVarContainer> class TVariables, class modelType>
class AbstractModel : public TVariables<TVarContainer>
{	
	template<typename> friend class VTKSnapshotter;
	template<typename> friend class AbstractSolver;
public:
	typedef TVarContainer VarContainer;
	typedef TVariables<TVarContainer> Variables;
	typedef mesh::TriangleMesh<VarContainer> Mesh;
	typedef typename Mesh::Cell Cell;
	typedef propsType Properties;
protected:
	std::shared_ptr<Mesh> mesh;
	std::shared_ptr<VTKSnapshotter<modelType>> snapshotter;

	// Spacial properties
	double length_perf;
	double r_w;
	double r_e;
	double Volume;
	size_t cellsNum;
	size_t varNum;
		
	// Rate of the well
	double Q_sum;
	double Pwf;
	// Ranges of perforated cells numbers
	std::vector<std::pair<int,int> > perfIntervals;
	// Vector of <cell number, rate in the cell> for left border cells
	std::map<size_t,double> Qcell;

	// Temporary properties
	double ht;
	double ht_min;
	double ht_max;
		
	// Number of periods
	size_t periodsNum;
	// End times of periods [sec]
	std::vector<double> period;
	// Oil rates [m3/day]
	std::vector<double> rate;
	// Vector of BHPs [bar]
	std::vector<double> pwf;
	// If left boundary condition would be 2nd type
	bool leftBoundIsRate;
	// If right boundary condition would be 1st type
	bool rightBoundIsPres;
	// BHP will be converted to the depth
	double depth_point;
	// During the time flow rate decreases 'e' times in well test [sec] 
	double alpha;
	double wellboreDuration;
	size_t skeletonsNum;

	virtual void loadMesh(const Task& task, const double height)
	{
		mesh = std::make_shared<Mesh>(task, height);
		cellsNum = mesh.get()->getCellsSize();
		varNum = VarContainer::size * cellsNum;
	}
	virtual void setProps(const propsType& props) = 0;
	virtual void makeDimLess() = 0;
	virtual void setPerforated()
	{
		Qcell[mesh->well_idx] = 0.0;
	};
	virtual void setInitialState() = 0;

	adouble linearAppr(const adouble a1, const double r1, const adouble a2, const double r2)
	{
		return (a1 * r2 + a2 * r1) / (r1 + r2);
	}

	static const int var_size;
public:
	AbstractModel() 
	{ 
		grav = 9.8;
	};
	virtual ~AbstractModel() {};
	
	// Dimensions
	double t_dim;
	double R_dim;
	double Z_dim;
	double P_dim;
	double T_dim;
	double Q_dim;
	double grav;
	
	void load(const Task& task, const Properties& props)
	{
		setProps(props);
		loadMesh(task, props.props_sk[0].height / props.R_dim);

		u_prev.resize(varNum);
		u_iter.resize(varNum);
		u_next.resize(varNum);

		setPerforated();
		setInitialState();
	};
	virtual void setPeriod(const int period) = 0;
	virtual void setWellborePeriod(int period, double cur_t) {};
	int getCellsNum() {	return cellsNum; };
	void snapshot_all(const int i) { snapshotter->dump(i); }
	const Mesh* getMesh() const
	{
		return mesh.get();
	}
	Mesh* getMesh()
	{
		return mesh.get();
	}
	void setSnapshotter(const modelType* model)
	{
		snapshotter = std::make_shared<VTKSnapshotter<modelType>>(model);
	}
};

#endif /* ABSTRACTMODEL_HPP_ */
