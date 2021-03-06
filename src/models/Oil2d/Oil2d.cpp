#include "src/models/Oil2d/Oil2d.hpp"
#include "src/util/utils.h"

#include "adolc/drivers/drivers.h"
#include "adolc/adolc.h"

using namespace oil2d;
using namespace std;

Oil2d::Oil2d()
{
}
Oil2d::~Oil2d()
{

	delete[] x, h;
}
void Oil2d::setProps(const Properties& props)
{
	R_dim = props.R_dim;
	r_w = props.r_w;
	r_e = props.r_e;
	perfIntervals = props.perfIntervals;

	leftBoundIsRate = props.leftBoundIsRate;
	rightBoundIsPres = props.rightBoundIsPres;

	skeletonsNum = props.props_sk.size();
	props_sk = props.props_sk;
	for (int j = 0; j < skeletonsNum; j++)
	{
		props_sk[j].kx = MilliDarcyToM2(props_sk[j].kx);
		props_sk[j].ky = MilliDarcyToM2(props_sk[j].ky);
	}

	periodsNum = props.timePeriods.size();
	for (int i = 0; i < periodsNum; i++)
	{
		period.push_back(props.timePeriods[i]);

		if (leftBoundIsRate)
			rate.push_back(props.rates[i] / 86400.0);
		else
			pwf.push_back(props.pwf[i]);
	}

	ht = props.ht;
	ht_min = props.ht_min;
	ht_max = props.ht_max;

	props_oil = props.props_oil;
	props_oil.visc = cPToPaSec(props_oil.visc);

	alpha = props.alpha;

	makeDimLess();
}
void Oil2d::makeDimLess()
{
	t_dim = 3600.0;
	P_dim = props_sk[0].p_init;

	// Temporal properties
	ht /= t_dim;
	ht_min /= t_dim;
	ht_max /= t_dim;

	// Grid properties
	r_w /= R_dim;
	r_e /= R_dim;

	for (auto& props : props_sk)
	{
		props.kx /= (R_dim * R_dim);
		props.ky /= (R_dim * R_dim);
		props.dens_stc /= (P_dim * t_dim * t_dim / R_dim / R_dim);
		props.beta /= (1.0 / P_dim);
		props.height /= R_dim;
		props.p_init /= P_dim;
		props.p_out /= P_dim;
	}

	Q_dim = R_dim * R_dim * R_dim / t_dim;
	for (int i = 0; i < periodsNum; i++)
	{
		period[i] /= t_dim;
		if (leftBoundIsRate)
			rate[i] /= Q_dim;
		else
			pwf[i] /= P_dim;
	}

	props_oil.visc /= (P_dim * t_dim);
	props_oil.dens_stc /= (P_dim * t_dim * t_dim / R_dim / R_dim);
	props_oil.beta /= (1.0 / P_dim);
	props_oil.p_ref /= P_dim;

	alpha /= t_dim;
}
void Oil2d::setInitialState()
{
	const auto& props = props_sk[0];
	for (size_t i = 0; i < cellsNum; i++)
	{
		const auto& cell = mesh->cells[i];
		auto data = (*this)[i];
		if(cell.type == CellType::FRAC)
			data.u_prev.p = data.u_iter.p = data.u_next.p = props.p_init;
		else
			data.u_prev.p = data.u_iter.p = data.u_next.p = props.p_init;
	}

	x = new TapeVariable[cellsNum];
	h = new adouble[var_size * cellsNum];
}
void Oil2d::setPeriod(const int period)
{
	if (leftBoundIsRate)
	{
		Q_sum = rate[period];

		/*if (period == 0 || rate[period - 1] < EQUALITY_TOLERANCE) {
			map<int, double>::iterator it;
			for (it = Qcell.begin(); it != Qcell.end(); ++it)
				it->second = Q_sum * cells[it->first].hz / height_perf;
		}
		else {
			map<int, double>::iterator it;
			for (it = Qcell.begin(); it != Qcell.end(); ++it)
				it->second = it->second * Q_sum / rate[period - 1];
		}*/
	}
	else
	{
		Pwf = pwf[period];
		Q_sum = 0.0;
	}
}
double Oil2d::getRate(const size_t cell_idx)
{
	return 0.0;
}

adouble Oil2d::solveInner(const Cell& cell)
{
	const auto& cur = x[cell.id];
	const auto prev = (*this)[cell.id].u_prev;

	adouble H = props_sk[0].getPoro(cur.p) * props_oil.getDensity(cur.p) - props_sk[0].getPoro(prev.p) * props_oil.getDensity(prev.p);
	for (int i = 0; i < 3; i++)
	{
		const int nebr_idx = cell.nebr[i];
		const auto& beta = mesh->cells[nebr_idx];
		const auto& nebr = x[nebr_idx];

		H += ht / cell.V * getTrans(cell, i, beta) *
			linearAppr(props_oil.getDensity(cur.p) / props_oil.getViscosity(cur.p), cell.dist[i],
				props_oil.getDensity(nebr.p) / props_oil.getViscosity(nebr.p), getDistance(beta, cell)) *
				(cur.p - nebr.p);
	}
	return H;
}
adouble Oil2d::solveBorder(const Cell& cell)
{
	const auto& cur = x[cell.id];
	const auto& nebr = x[cell.nebr[0]];
	adouble H;
	adouble rightIsPres = rightBoundIsPres;
	condassign(H, rightIsPres, (cur.p - (adouble)(props_sk[0].p_out)) / P_dim, (cur.p - nebr.p) / P_dim);

	return H;
}
adouble Oil2d::solveWell(const Cell& cell)
{
	const auto& cur = x[cell.id];
	const auto prev = (*this)[cell.id].u_prev;

	adouble H = props_sk[0].getPoro(cur.p) * props_oil.getDensity(cur.p) - props_sk[0].getPoro(prev.p) * props_oil.getDensity(prev.p);
	for (int i = 0; i < mesh->wellNebrs.size(); i++)
	{
		const auto& nebr_str = mesh->wellNebrs[i];
		const int nebr_idx = nebr_str.id;
		const auto& beta = mesh->cells[nebr_idx];
		const auto& nebr = x[nebr_idx];

		H += ht / mesh->well_vol * getTrans(cell, i, beta) *
			linearAppr(props_oil.getDensity(cur.p) / props_oil.getViscosity(cur.p), nebr_str.dist,
				props_oil.getDensity(nebr.p) / props_oil.getViscosity(nebr.p), getDistance(beta, cell)) *
				(cur.p - nebr.p);
	}

	return H;
}