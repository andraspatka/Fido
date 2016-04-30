#include "../include/FidoControlSystem.h"

#include <cassert>

#include "../include/NeuralNet.h"
#include "../include/Backpropagation.h"
#include "../include/Adadelta.h"
#include "../include/Pruner.h"
#include "../include/LSInterpolator.h"

using namespace rl;

FidoControlSystem::FidoControlSystem(int stateDimensions, Action minAction, Action maxAction, int baseOfDimensions) : WireFitQLearn(stateDimensions, minAction.size(), 1, 12, 5, minAction, maxAction, baseOfDimensions, new rl::LSInterpolator(), new net::Adadelta(0.95, 0.1, 10000), 1, 0)  {
	explorationLevel = initialExploration;
}


std::vector<double> FidoControlSystem::chooseBoltzmanActionDynamic(State state) {
	return WireFitQLearn::chooseBoltzmanAction(state, explorationLevel);
}

void FidoControlSystem::applyReinforcementToLastAction(double reward, State newState) {
	std::vector<Wire> newContolWires = newControlWiresForHistory(History(lastState, newState, lastAction, reward));

	double uncertainty = getError(lastState, getRawOutput(newContolWires));
	adjustExploration(uncertainty);
	lastUncertainty = uncertainty;

	std::vector<History> selectedHistories = selectHistories();
	trainOnHistories(selectedHistories);

	std::vector< std::vector<double> > input;
	std::vector< std::vector<double> > correctOutput;
	inputOutputForHistories(selectedHistories, &input, &correctOutput);

	trainer->train(network, input, correctOutput);

	histories.push_back(History(lastState, newState, lastAction, reward));
	if(histories.size() > 200) {
		histories.erase(histories.begin());
	}
}

void FidoControlSystem::reset() {
	std::cout << "RESET\n";
	WireFitQLearn::reset();
	histories.clear();
	explorationLevel = initialExploration;
}


double FidoControlSystem::getError(std::vector<double> input, std::vector<double> correctOutput) {
	double totalError = 0;
	std::vector<double> output = network->getOutput(input);
	for(unsigned int a = 0; a < output.size(); a++) {
		totalError += pow(output[a] - correctOutput[a], 2);
	}

	return totalError / (double)output.size();
}

void FidoControlSystem::trainOnHistories(std::vector<FidoControlSystem::History> selectedHistories) {
	if(selectedHistories.size() < 1) {
		return;
	}

	std::vector< std::vector<double> > input;
	std::vector< std::vector<double> > correctOutput;
	inputOutputForHistories(selectedHistories, &input, &correctOutput);

	if(selectedHistories.size() > 3) {
		bool didChange = false;
		net::Pruner pruner;
		net::Adadelta testTrainer = net::Adadelta(0.95, 0.2, 100);
		net::NeuralNet originalNet;
		int iter = 0;
		while(iter < 2) {
			iter++;
			for(net::Layer &l : network->net) {
				for(net::Neuron &n : l.neurons) {
					n.randomizeWeights();
				}
			}

			originalNet = net::NeuralNet(network);
			double totalCurrentError = testTrainer.train(network, input, correctOutput);

			pruner.prune(network, &testTrainer);
			double totalPrunedError = testTrainer.train(network, input, correctOutput);

			if(totalPrunedError*1.05 < totalCurrentError) {
				didChange = true;
				continue;
			}

			*network = originalNet;
			network->net[0].neurons.push_back(net::Neuron(network->numberOfInputs()));
			for(net::Neuron &n : network->net[1].neurons) {
				n.weights.push_back(1);
				n.randomizeWeights();
			}
			double totalAddedError = testTrainer.train(network, input, correctOutput);
			if(totalAddedError*1.05 < totalCurrentError) {
				didChange = true;
				continue;
			}

			*network = originalNet;
			break;
		}

		if(didChange == false) {
			*network = originalNet;
		}
	}

	trainer->train(network, input, correctOutput);

	std::cout << "Num neurons: " << network->numberOfHiddenNeurons() << "\n";
}

void FidoControlSystem::adjustExploration(double uncertainty) {
	explorationLevel = pow(uncertainty, 2) * 10000;
	if(explorationLevel < 0.00001) {
		explorationLevel = 0.00001;
	}
	if(explorationLevel > 1000) {
		explorationLevel = 1000;
	}
	std::cout << "Exploration level: " << explorationLevel << "\n";
}

std::vector<FidoControlSystem::History> FidoControlSystem::selectHistories() {
	std::vector<History> selectedHistories;
	std::vector<History> tempHistories(histories);
	while(selectedHistories.size() < samplesOfHistory && tempHistories.size() > 0) {
		auto bestHistory = tempHistories.end()-1;

		selectedHistories.push_back(*bestHistory);
		tempHistories.erase(bestHistory);
	}

	return selectedHistories;
}

std::vector<Wire> FidoControlSystem::newControlWiresForHistory(History history) {
	std::vector<Wire> controlWires = getWires(history.initialState);
	double newRewardForLastAction = getQValue(history.reward, history.initialState, history.newState, history.action, controlWires);
	Wire correctWire = {history.action, newRewardForLastAction};
	return WireFitQLearn::newControlWires(correctWire, controlWires);
}

void FidoControlSystem::inputOutputForHistories(std::vector<FidoControlSystem::History> histories, std::vector< std::vector<double> > * input, std::vector< std::vector<double> > * correctOutput) {
	std::map<State, std::vector<History>> historiesByState;

	for(const History &history : histories) {
		bool foundState = false;
		for(const auto &historiesStatePair : historiesByState) {
			if(historiesStatePair.first == history.initialState) {
				foundState = true;
				break;
			}
		}
		if(foundState == false) {
			historiesByState[history.initialState] = {history};
		} else {
			historiesByState[history.initialState].push_back(history);
		}
	}

	for(const auto &historiesStatePair : historiesByState) {
		std::vector<Wire> correctWires;
		for(History history : historiesStatePair.second) {
			std::vector<Wire> controlWires = getWires(history.initialState);
			double newRewardForLastAction = getQValue(history.reward, history.initialState, history.newState, history.action, controlWires);
			correctWires.push_back({history.action, newRewardForLastAction});
		}

		input->push_back(historiesStatePair.first);
		correctOutput->push_back(getRawOutput(WireFitQLearn::newControlWires(correctWires, getWires(historiesStatePair.first))));
	}
}
