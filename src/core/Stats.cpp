/*
Copyright (C) 2018 Adrian Michel
http://www.amichel.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "positions.h"
#include <boost\shared_array.hpp>
#include <algorithm>

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

void SlippageStats::onPosition(tradery::Position pos) {
  if (pos.isClosed()) {
    this->_totalCloseSlippage += pos.getCloseSlippage();
  }
  _totalOpenSlippage += pos.getEntrySlippage();
}

void CommissionStats::onPosition(tradery::Position pos) {
  if (pos.isClosed()) {
    _totalCloseCommission += pos.getCloseCommission();
  }
  _totalOpenCommission += pos.getEntryCommission();
}

void PosStats::calc(tradery::Position pos, double gain, double posPctGain) {
  _gain += gain;
  _count++;

  if (gain > 0) {
    _winningCount++;
    _totalGain += gain;
    //    COUT << _T( "total gain: " ) << _totalGain << std::endl;
  } else if (gain < 0) {
    _losingCount++;
    _totalLoss += gain;
    //    COUT << _T( "total loss: " ) << _totalLoss << std::endl;
  } else
    _neutralCount++;

  _maxGainPerPos = std::max(_maxGainPerPos, gain);
  _maxLossPerPos = std::min(_maxLossPerPos, gain);

  _maxPctGainPerPos = std::max(_maxPctGainPerPos, posPctGain);
  _maxPctLossPerPos = std::min(_maxPctLossPerPos, posPctGain);

  _totalPctGain += posPctGain;

  _totalOpenCost += pos.getEntryCost();
}

void ClosedPosStats::onPosition(tradery::Position pos) {
  if (pos.isClosed()) {
    this->_commissionStats.onPosition(pos);
    this->_slippageStats.onPosition(pos);

    double gain = pos.getGain();
    double pctGain = pos.getPctGain();

    calc(pos, gain, pctGain);
    _totalCloseCost += pos.getCloseIncome();
  }
}

void OpenPosStats::onPosition(tradery::Position pos,
                              const CurrentPriceSource& cpr) {
  //  std::cout << _T( "OpenPosStats::onPosition - 1" ) << std::endl;
  if (pos.isOpen()) {
    //    std::cout << _T( "OpenPosStats::onPosition - 2" ) << std::endl;
    this->_commissionStats.onPosition(pos);
    this->_slippageStats.onPosition(pos);

    //    std::cout << _T( "OpenPosStats::onPosition - 3" ) << std::endl;
    double gain = 0;

    try {
      // this is the gain considering the last price of the security for an open
      // position
      const std::string& symbol(pos.getSymbol());
      //      std::cout << _T( "OpenPosStats::onPosition - 4" ) << std::endl;
      gain += pos.getGain(cpr.get(symbol));
      //      std::cout << _T( "OpenPosStats::onPosition - 5" ) << std::endl;
    } catch (const DataNotAvailableForSymbolException&) {
      // ignore symbols for which we can't get data
      // todo: should not happen - if we could open a position, we should be
      // able to get data for it
    }
    double pctGain = gain / pos.getEntryCost() * 100;
    //    std::cout << _T( "OpenPosStats::onPosition - 8" ) << std::endl;

    calc(pos, gain, pctGain);
    //    std::cout << _T( "OpenPosStats::onPosition - 10" ) << std::endl;
  }
}

StatsCalculator::StatsCalculator(
    const CurrentPriceSource& currentPriceRequester)
    : _cpr(currentPriceRequester) {}

void StatsCalculator::calculate(
    PositionsContainer& positions,
    const PositionEqualPredicate& positionPredicate) {
  reset();
  positions.forEach(*this, positionPredicate);
}

void StatsCalculator::calculateAll(PositionsContainer& positions) {
  //  COUT << std::endl;
  reset();
  positions.forEach(*this);
}

void StatsCalculator::calculateLong(PositionsContainer& positions) {
  calculate(positions, PositionEqualLongPredicate());
}

void StatsCalculator::calculateShort(PositionsContainer& positions) {
  calculate(positions, PositionEqualShortPredicate());
}

void StatsCalculator::onPosition(tradery::Position pos) {
  if (pos.isClosed())
    _closedPosStats.onPosition(pos);
  else
    _openPosStats.onPosition(pos, _cpr);

  _allPosStats = _openPosStats + _closedPosStats;
}

class Gain : public PositionHandler {
 private:
  double _gain;

 public:
  Gain() : _gain(0) {}

  virtual void onPositionConst(const tradery::Position pos) {
    if (pos.isClosed()) _gain += pos.getGain();
  }

  double operator()() const { return _gain; }
};

inline double totalGain(const PositionsContainer& pos) {
  Gain gain;
  pos.forEachConst(gain);
  return gain();
}

inline double averageGain(const PositionsContainer& pos) {
  Gain gain;

  pos.forEachConst(gain);
  return gain() / (pos.enabledCount() - pos.openPositionsCount());
}

// two sinchronized series: times and equity values
// make it a series, so we can apply all the indicators and other stats
class EquitySeries : public std::vector<double>, public PositionHandler {
 private:
  typedef std::map<DateTime, double> TimeToDoubleMap;

 private:
  TimeSeries _time;
  const double _initialEquity;
  TimeToDoubleMap _m;

 public:
  EquitySeries(const PositionsContainer& pos) : _initialEquity(0) { init(pos); }

  EquitySeries(const PositionsContainer& pos, double initialEquity)
      : _initialEquity(initialEquity) {
    init(pos);
  }

  void init(const PositionsContainer& pos) {
    pos.forEachClosedConst(*this);

    double equity = _initialEquity;
    for (TimeToDoubleMap::const_iterator i = _m.begin(); i != _m.end(); i++) {
      equity += i->second;
      // set the time
      _time.push_back(i->first);
      // set total equity for the time
      push_back(equity);
    }
  }

  // called for each closed position in the pos passed to the constructor
  virtual void onPositionConst(const tradery::Position pos) {
    assert(pos.isClosed());

    // populate the map with gain/bar
    TimeToDoubleMap::iterator i = _m.find(pos.getCloseTime());
    if (i == _m.end())
      _m.insert(TimeToDoubleMap::value_type(pos.getCloseTime(), pos.getGain()));
    else
      i->second += pos.getGain();
  }

  // the final equity value - initial equity;
  double totalProfit() const { return back() - _initialEquity; }

  // total profit / number of bars (known externally)
  // TODO: see how I can get this right
  double profitPerBar(size_t bars) const { return totalProfit() / bars; }
};
