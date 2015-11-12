/*
 * artery_options.cxx
 *
 *  Created on: Dec 22, 2014
 *      Author: cgg
 */


#include "artery.hxx"

using namespace std;
namespace po = boost::program_options;

// default constructor for arteryTree
arteryTree::arteryTree(po::variables_map o, arteryTreeInit *init):
	randGen(init->seed),
	u01(randGen),
	lengthDist(o["vesselSeg.lengthBetaA"].as<double>(),o["vesselSeg.lengthBetaB"].as<double>()),
	radiusDist(o["vesselSeg.radiusBetaA"].as<double>(),o["vesselSeg.radiusBetaB"].as<double>()){

		opt = o;

		// assign id and update number of arteries
		id = num;
		num += 1;

		fill = vtkImageData::New();
		double spacing[3];
		for(int i=0; i<3; i++){
			spacing[i] = (init->endPos[i] - init->startPos[i])/(init->nFill[i]);
		}
		fill->SetSpacing(spacing);
		fill->SetExtent(0, init->nFill[0]-1, 0, init->nFill[1]-1, 0, init->nFill[2]-1);
		double origin[3];
		for(int i=0; i<3; i++){
			origin[i] = init->startPos[i]+spacing[i]/2.0;
		}
		fill->SetOrigin(origin);

#if VTK_MAJOR_VERSION <= 5
		fill->SetNumberOfScalarComponents(1);
		fill->SetScalarTypeToDouble();
		fill->AllocateScalars();
#else
		fill->AllocateScalars(VTK_DOUBLE,1);
#endif

		numBranch = 0;
		maxBranch = o["vesselTree.maxBranch"].as<uint>();
		boundBox = init->boundBox;
		tissue = init->tissue;
		for(int i=0; i<3; i++){
			prefDir[i] = init->prefDir[i];
		}
		breast = init->breast;

		// temporarily set head branch pointer
		head = nullptr;
	}

// destructor
arteryTree::~arteryTree(){
	fill->Delete();
}


// constructor for first branch (the root)
arteryBr::arteryBr(double* spos, double* sdir, double r, arteryTree *owner){


	double pos[3];
	int invox[3];

	bool failSeg = false;	// failed to create a valid segment
	bool edgeSeg = false;	// segment at boundary of ROI

	for(int i=0; i<3; i++){
		startPos[i] = spos[i];
		startDir[i] = sdir[i];
	}
	startRad = r;
	myTree = owner;

	// no parent or sibling branches
	parent = nullptr;
	prevBranch = nullptr;
	nextBranch = nullptr;

	// root branch has id 0 and level 0 and generation 0
	id = 0;
	level = 0;
	gen = 0;

	// increment tree branch count
	myTree->numBranch += 1;

	// determine length of branch
	length = setLength();
	curLength = 0.0;

	// generate segments to fill branch
	firstSeg = new arterySeg(this);
	lastSeg = firstSeg;

	// update length
	curLength += firstSeg->length;

	if (firstSeg->length == 0.0){
		failSeg = true;
	}

	// check if at ROI boundary by seeing if any neighboring voxels are outside ROI
	double* thePos = lastSeg->endPos;
	double pcoords[3];
	myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
	for(int a=-1; a<=1; a++){
		for(int b=-1; b<=1; b++){
			for(int c=-1; c<=1; c++){
				unsigned char* p =
					static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
				if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg){
					edgeSeg = true;
				}
			}
		}
	}

	// generate more segments until proper length
	while(curLength < length && !failSeg && !edgeSeg){
		lastSeg->nextSeg = new arterySeg(lastSeg);  // the lastSeg in parenthesis is used to fill variables including prevSeg ptr
		lastSeg = lastSeg->nextSeg;
		curLength += lastSeg->length;
		if(lastSeg->length == 0.0){
			failSeg = true;
		}
		// check if at ROI boundary
		thePos = lastSeg->endPos;
		myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
		for(int a=-1; a<=1; a++){
			for(int b=-1; b<=1; b++){
				for(int c=-1; c<=1; c++){
					unsigned char* p =
						static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
					if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg){
						edgeSeg = true;
					}
				}
			}
		}
	}

	// fill in end of branch variables
	for(int i=0; i<3; i++){
		endPos[i] = lastSeg->endPos[i];
		endDir[i] = lastSeg->endDir[i];
	}
	endRad = lastSeg->endRad;

	length = curLength;

	// set number of children and generate them
	nChild = setChild();

	// debug
	//cout << "br " << id << " len = " << length << " lev = " << level << " gen = " << gen <<  " child = " << nChild << "\n";

	if(failSeg){
		nChild = 0;
		std::cout << "Segment generation failure for branch" << id << std::endl;
	}

	if(edgeSeg){
		nChild = 0;
		std::cout << "ROI edge collision for branch " << id << std::endl;
	}

	if (nChild == 0){
		firstChild = nullptr;
		lastChild = nullptr;
	} else {
		// pick radii
		double* radii = (double*)malloc(nChild*sizeof(double));
		setRadii(radii);
		// setup first child with level equal to current level
		firstChild = new arteryBr(this,level,gen+1,radii[0]);
		lastChild = firstChild;
		for(int i=1; i < nChild; i++){
			// set up other children with higher level than current
			lastChild->nextBranch = new arteryBr(this,lastChild,level+1,gen+1,radii[i]);
			lastChild = lastChild->nextBranch;
		}
		free(radii);
	}
}

// constructor for first child branch of a parent branch
arteryBr::arteryBr(arteryBr* par, unsigned int lev, unsigned int g, double r){

	bool failSeg = false;
	bool edgeSeg = false;
	int invox[3];

	// pointers
	parent = par;
	prevBranch = nullptr;
	nextBranch = this;

	for(int i=0; i<3; i++){
		startPos[i] = parent->endPos[i];
	}
	startRad = r;
	level = lev;
	gen = g;
	myTree = parent->myTree;
	id = myTree->numBranch;
	myTree->numBranch += 1;

	// set starting direction same a parent endDir for first child
	for(int i=0; i<3; i++){
		startDir[i] = parent->endDir[i];
	}

	// determine length of branch
	length = setLength();
	curLength = 0.0;

	// generate segments to fill branch
	firstSeg = new arterySeg(this);
	lastSeg = firstSeg;

	// update length
	curLength += firstSeg->length;

	if (firstSeg->length == 0.0){
		failSeg = true;
	}

	// check if at ROI boundary
	double* thePos = lastSeg->endPos;
	double pcoords[3];
	myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
	for(int a=-1; a<=1; a++){
		for(int b=-1; b<=1; b++){
			for(int c=-1; c<=1; c++){
				unsigned char* p =
					static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
				if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg || p[0] == myTree->tissue->muscle){
					edgeSeg = true;
				}
			}
		}
	}

	// generate more segments until proper length
	while(curLength < length && !failSeg && !edgeSeg){
		lastSeg->nextSeg = new arterySeg(lastSeg);  // the lastSeg in parenthesis is used to fill variables including prevSeg ptr
		lastSeg = lastSeg->nextSeg;
		curLength += lastSeg->length;
		if(lastSeg->length == 0.0){
			failSeg = true;
		}
		// check if at ROI boundary
		thePos = lastSeg->endPos;
		myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
		for(int a=-1; a<=1; a++){
			for(int b=-1; b<=1; b++){
				for(int c=-1; c<=1; c++){
					unsigned char* p =
						static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
					if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg || p[0] == myTree->tissue->muscle){
						edgeSeg = true;
					}
				}
			}
		}
	}

	// fill in end of branch variables
	for(int i=0; i<3; i++){
		endPos[i] = lastSeg->endPos[i];
		endDir[i] = lastSeg->endDir[i];
	}
	endRad = lastSeg->endRad;

	length = curLength;

	// set number of children and generate them
	nChild = setChild();

	// debug
	//cout << "br " << id << " len = " << length << " lev = " << level << " gen = " << gen <<  " child = " << nChild << "\n";

	if(failSeg){
		nChild = 0;
		std::cout << "Segment generation failure for branch" << id << std::endl;
	}

	if(edgeSeg){
		nChild = 0;
		std::cout << "ROI edge collision for branch " << id << std::endl;
	}

	if (nChild == 0){
		firstChild = nullptr;
		lastChild = nullptr;
	} else {
		// pick radii
		double* radii = (double*)malloc(nChild*sizeof(double));
		setRadii(radii);
		// setup first child with level equal to current level
		firstChild = new arteryBr(this,level,gen+1,radii[0]);
		lastChild = firstChild;
		for(int i=1; i < nChild; i++){
			// set up other children with higher level than current
			lastChild->nextBranch = new arteryBr(this,lastChild,level+1,gen+1,radii[i]);
			lastChild = lastChild->nextBranch;
		}
		free(radii);
	}
}

// constructor for subsequent children (not first child) of a parent branch
arteryBr::arteryBr(arteryBr* par, arteryBr* par2, unsigned int lev, unsigned int g, double r){

	bool failSeg = false;
	bool edgeSeg = false;

	int invox[3];

	// pointers
	parent = par;
	prevBranch = par2;
	nextBranch = this;

	for(int i=0; i<3; i++){
		startPos[i] = parent->endPos[i];
	}
	startRad = r;
	level = lev;
	gen = g;
	myTree = parent->myTree;
	id = myTree->numBranch;
	myTree->numBranch += 1;

	// set starting direction
	setDir(startDir);

	// determine length of branch
	length = setLength();
	curLength = 0.0;

	// generate segments to fill branch
	firstSeg = new arterySeg(this);
	lastSeg = firstSeg;

	// update length
	curLength += firstSeg->length;

	if (firstSeg->length == 0.0){
		failSeg = true;
	}

	// check if at ROI boundary
	double* thePos = lastSeg->endPos;
	double pcoords[3];
	myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
	for(int a=-1; a<=1; a++){
		for(int b=-1; b<=1; b++){
			for(int c=-1; c<=1; c++){
				unsigned char* p =
					static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
				if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg || p[0] == myTree->tissue->muscle){
					edgeSeg = true;
				}
			}
		}
	}

	// generate more segments until proper length
	while(curLength < length && !failSeg && !edgeSeg){
		lastSeg->nextSeg = new arterySeg(lastSeg);  // the lastSeg in parenthesis is used to fill variables including prevSeg ptr
		lastSeg = lastSeg->nextSeg;
		curLength += lastSeg->length;
		if(lastSeg->length == 0.0){
			failSeg = true;
		}
		// check if at ROI boundary
		thePos = lastSeg->endPos;
		myTree->breast->ComputeStructuredCoordinates(thePos, invox, pcoords);
		for(int a=-1; a<=1; a++){
			for(int b=-1; b<=1; b++){
				for(int c=-1; c<=1; c++){
					unsigned char* p =
						static_cast<unsigned char*>(myTree->breast->GetScalarPointer(invox[0]+a,invox[1]+b,invox[2]+c));
					if(p[0] == myTree->tissue->skin || p[0] == myTree->tissue->bg || p[0] == myTree->tissue->muscle){
						edgeSeg = true;
					}
				}
			}
		}
	}

	// fill in end of branch variables
	for(int i=0; i<3; i++){
		endPos[i] = lastSeg->endPos[i];
		endDir[i] = lastSeg->endDir[i];
	}
	endRad = lastSeg->endRad;

	length = curLength;

	// set number of children and generate them
	nChild = setChild();

	// debug
	//cout << "br " << id << " len = " << length << " lev = " << level << " gen = " << gen <<  " child = " << nChild << "\n";

	if(failSeg){
		nChild = 0;
		std::cout << "Segment generation failure for branch" << id << std::endl;
	}

	if(edgeSeg){
		nChild = 0;
		std::cout << "ROI edge collision for branch " << id << std::endl;
	}

	if (nChild == 0){
		firstChild = nullptr;
		lastChild = nullptr;
	} else {
		// pick radii
		double* radii = (double*)malloc(nChild*sizeof(double));
		setRadii(radii);
		// setup first child with level equal to current level
		firstChild = new arteryBr(this,level,gen+1,radii[0]);
		lastChild = firstChild;
		for(int i=1; i < nChild; i++){
			// set up other children with higher level than current
			lastChild->nextBranch = new arteryBr(this,lastChild,level+1,gen+1,radii[i]);
			lastChild = lastChild->nextBranch;
		}
		free(radii);
	}
}

double arteryBr::setLength(void){
	// set length using random distribution and level
	double minLen0 = myTree->opt["vesselBr.minLen0"].as<double>();
	double maxLen0 = myTree->opt["vesselBr.maxLen0"].as<double>();
	double minLen1 = myTree->opt["vesselBr.minLen1"].as<double>();
	double maxLen1 = myTree->opt["vesselBr.maxLen1"].as<double>();
	double minLen2 = myTree->opt["vesselBr.minLen2"].as<double>();
	double maxLen2 = myTree->opt["vesselBr.maxLen2"].as<double>();
	double minLenDefault = myTree->opt["vesselBr.minLenDefault"].as<double>();
	double maxLenDefault = myTree->opt["vesselBr.maxLenDefault"].as<double>();

	double len;
	double randVal = myTree->u01();

	if(level == 0){
		len = minLen0 + randVal*(maxLen0-minLen0);
	} else if(level == 1){
		len = minLen1 + randVal*(maxLen1-minLen1);
	} else if(level == 2){
		len = minLen2 + randVal*(maxLen2-minLen2);
	} else {
		len = minLenDefault + randVal*(maxLenDefault-minLenDefault);
	}
	return(len);
}

unsigned int arteryBr::setChild(void){
	// determine number of child branches
	// cumulative probabilities of having 0-4 children based on level
	unsigned int maxChild = myTree->opt["vesselBr.maxChild"].as<uint>();
	unsigned int levBound = myTree->opt["vesselBr.childLevBound"].as<uint>();
	ostringstream var;

	double *prob = (double *)malloc((levBound+1)*maxChild*sizeof(double));

	for(int a=0; a<=levBound; a++){
		for(int b=0; b<maxChild; b++){
			var.str("");
			var.clear();
			var << "vesselBr.child" << a << b;
			prob[a*maxChild+b] = myTree->opt[var.str()].as<double>();
		}
	}

	// if small enough, no children
	double minRad = myTree->opt["vesselBr.childMinRad"].as<double>();
	if(endRad < minRad){
		free(prob);
		return(0);
	}

	// check for max number branches
	if(myTree->numBranch >= myTree->maxBranch){
		free(prob);
		return(0);
	}

	// define maximum generation
	unsigned int maxGen = myTree->opt["vesselTree.maxGen"].as<uint>();
	if(gen > maxGen){
		free(prob);
		return(0);
	}

	double randVal = myTree->u01();

	for(int b=maxChild-1; b >= 0; b--){
		if(randVal > prob[std::min(level,levBound)*maxChild+b]){
			free(prob);
			return(b+1);
		}
	}
	// default to no children
	free(prob);
	return(0);
}

void arteryBr::setRadii(double* radii){
	// set radii of child branches based on radii of the parent

	double minFrac = myTree->opt["vesselBr.minRadFrac"].as<double>();
	double maxFrac = myTree->opt["vesselBr.maxRadFrac"].as<double>();

	double radFrac0 = myTree->opt["vesselBr.radFrac0"].as<double>();

	double randVal;

	// main child same radius as parent
	radii[0] = endRad*radFrac0;

	for(unsigned int n=1; n<nChild; n++){
		randVal = myTree->u01();
		radii[n] = (minFrac + randVal*(maxFrac-minFrac))*endRad;
	}
}

void arteryBr::setDir(double* sdir){

	const double pi = boost::math::constants::pi<double>();

	// set initial direction of child branches (other than first child)
	double dir[3];
	double tempV[3];
	double basis1[3];
	double basis2[3];

	// minimum angle between parent direction and new branch
	double angleMin = pi*(myTree->opt["vesselBr.minAngle"].as<double>());
	double angleMax = pi*(myTree->opt["vesselBr.maxAngle"].as<double>());

	double randVal = myTree->u01();
	double angle = angleMin + randVal*(angleMax - angleMin);

	// random rotation about parent direction
	double rotate = 2*pi*myTree->u01();

	// project origin onto plane perpendicular parent endDir
	double dotProd = 0.0;
	for(int i=0; i<3; i++){
		dotProd += parent->endDir[i]*startPos[i];
	}
	for(int i=0; i<3; i++){
		tempV[i] = dotProd*parent->endDir[i];
		basis1[i] = tempV[i] - startPos[i];
	}

	// normalize basis1
	double norm = 0.0;
	for(int i=0; i<3; i++){
		norm += basis1[i]*basis1[i];
	}
	norm = sqrt(norm);
	for(int i=0; i<3; i++){
		basis1[i] = basis1[i]/norm;
	}

	// find second basis vector using cross product
	basis2[0] = parent->endDir[1]*basis1[2] - parent->endDir[2]*basis1[1];
	basis2[1] = parent->endDir[2]*basis1[0] - parent->endDir[0]*basis1[2];
	basis2[2] = parent->endDir[0]*basis1[1] - parent->endDir[1]*basis1[0];

	for(int i=0; i<3; i++){
		sdir[i] = cos(angle)*parent->endDir[i] + sin(angle)*(cos(rotate)*basis1[i] + sin(rotate)*basis2[i]);
	}
}


// branch destructor that also deletes all child branches
arteryBr::~arteryBr(){
	// delete segments
	arterySeg* delSeg;
	while(firstSeg != lastSeg){
		delSeg = firstSeg;
		firstSeg = firstSeg->nextSeg;
		delete(delSeg);
	}
	delete(firstSeg);

	// delete child branches
	arteryBr* delBranch;
	while(firstChild != lastChild){
		delBranch = firstChild;
		firstChild = firstChild->nextBranch;
		delete(delBranch);
	}
	delete(firstChild);
}

// constructor for first segment
arterySeg::arterySeg(arteryBr* br){
	myBranch = br;
	prevSeg = nullptr;
	nextSeg = this;

	for(int i=0; i<3; i++){
		startPos[i] = myBranch->startPos[i];
		startDir[i] = myBranch->startDir[i];
	}
	startRad = myBranch->startRad;

	// keeping derivatives zero at nodes for now
	startDeriv = 0.0;

	// code to generate random segment
	makeSeg();
}

// constructor for subsequent segments
arterySeg::arterySeg(arterySeg* pr){
	prevSeg = pr;
	myBranch = prevSeg->myBranch;
	nextSeg = this;

	for(int i=0; i<3; i++){
		startPos[i] = prevSeg->endPos[i];
		startDir[i] = prevSeg->endDir[i];
	}

	startRad = prevSeg->endRad;
	startDeriv = prevSeg->endDeriv;

	// code to generate random segment
	makeSeg();
}

void arterySeg::makeSeg(){

	const double pi = boost::math::constants::pi<double>();

	double minLen = myBranch->myTree->opt["vesselSeg.minLen"].as<double>();
	double maxLen = myBranch->myTree->opt["vesselSeg.maxLen"].as<double>();
	unsigned int numTry = myBranch->myTree->opt["vesselSeg.numTry"].as<uint>();
	unsigned int maxTry = myBranch->myTree->opt["vesselSeg.maxTry"].as<uint>();
	unsigned int absMaxTry = myBranch->myTree->opt["vesselSeg.absMaxTry"].as<uint>();
	double maxRad = myBranch->myTree->opt["vesselSeg.maxCurvRad"].as<double>();
	double angleMax =  pi*myBranch->myTree->opt["vesselSeg.maxCurvFrac"].as<double>();
	double roiStep = myBranch->myTree->opt["vesselSeg.roiStep"].as<double>();
	double densityWt = myBranch->myTree->opt["vesselSeg.densityWt"].as<double>();
	double angleWt = myBranch->myTree->opt["vesselSeg.angleWt"].as<double>();
	double prefDir[3]; // preferential direction of growth
	for(int i=0; i<3; i++){
		prefDir[i] = myBranch->myTree->prefDir[i];
	}
	double maxEndRad = myBranch->myTree->opt["vesselSeg.maxEndRad"].as<double>();
	double minEndRad = myBranch->myTree->opt["vesselSeg.minEndRad"].as<double>();

	int fillExtent[6];	// extents of fill
	myBranch->myTree->fill->GetExtent(fillExtent);

	double pos[3];
	unsigned int invox[3];

	unsigned int curTry;		// number of valid segments tested so far
	unsigned int totalTry;		// number of test segments so far
	unsigned int allTry;		// total number of test segments overall

	double lengthLB, lengthUB;  // bounds on random length
	double randVal, quantileVal;  // for length random generator

	double theta;  				// rotation of segment
	double radius; 				// segment radius of curvature
	double radLB,radUB; 		// min and max radius of curvature

	double curv[3];  			// point of rotation
	double curvNorm;			// stores norm(startPos-curv)
	double basis1[3];
	double basis2[3]; 			// basis vectors
	double tempV[3];
	double checkPos[3];			// checking if segment position in ROI

	int myVoxel[3];
	double pcoords[3];
	double checkAngle;
	double checkLength;
	double currentDir[3];  // unit vector in current direction
	double angleStep; // angular step size for checking in ROI for segment

	bool foundSeg = false;  // have we found a good segment yet?
	bool inROI;		// is current test segment in ROI?
	bool inFOV;		// is current segment in FOV?

	// breast FOV for checking if segment in FOV
	double breastOrigin[3];
	double breastSpacing[3];
	int breastDim[3];
	double breastFOV[6];

	myBranch->myTree->breast->GetOrigin(breastOrigin);
	myBranch->myTree->breast->GetSpacing(breastSpacing);
	myBranch->myTree->breast->GetDimensions(breastDim);

	for(int i=0; i<3; i++){
		breastFOV[2*i] = breastOrigin[i];
		breastFOV[2*i+1] = breastOrigin[i]+(double)breastDim[i]*breastSpacing[i];
	}

	double bestCurv[3];	// best center of curvature found so far
	double bestRadius;
	double bestCost;  // best cost found so far
	double cost;		// current cost

	double density;	// change in density of artery structure due to new segment

	// determine proposed segment length
	if (maxLen < (myBranch->length - myBranch->curLength)/10.0){
		length = maxLen;
	} else if (minLen > (myBranch->length - myBranch->curLength)) {
		length = minLen;
	} else {
		// use beta distribution to choose random length
		// beta distribution bounds;
		lengthLB = fmax(minLen,(myBranch->length - myBranch->curLength)/10.0);
		lengthUB = fmin(maxLen,(myBranch->length - myBranch->curLength));

		// generate a random length using beta distribution
		randVal = myBranch->myTree->u01();
		quantileVal = boost::math::quantile(myBranch->myTree->lengthDist, randVal);
		// scale to length range
		length = quantileVal*(lengthUB-lengthLB) + lengthLB;
	}

	allTry = 0;

	while (!foundSeg && allTry < absMaxTry){
		curTry = 0;
		while (curTry < numTry){
			totalTry = 0;
			inROI = false;
			inFOV = false;
			while (!inROI && !inFOV && totalTry < maxTry){
				allTry++;
				// generate random segment
				theta = 2*pi*myBranch->myTree->u01();
				radUB = maxRad;
				radLB = length/angleMax;
				// use beta distribution to pick radius
				randVal = myBranch->myTree->u01();
				quantileVal = boost::math::quantile(myBranch->myTree->radiusDist, randVal);
				// scale to radius range
				radius = quantileVal*(radUB-radLB) + radLB;
				totalTry += 1;

				// checking if in ROI
				// need basis vectors in plane perpendicular to startDir

				// project origin (0,0,0) onto plane perpendicular to startDir
				vtkMath::ProjectVector(startPos, startDir, tempV);

				vtkMath::Subtract(tempV, startPos, basis1);

				// normalize it
				vtkMath::Normalize(basis1);

				// find second basis vector using cross product
				vtkMath::Cross(startDir,basis1,basis2);

				// calculate curvature
				for(int i=0; i<3; i++){
					curv[i] = startPos[i] + radius*(basis1[i]*cos(theta) + basis2[i]*sin(theta));
				}

				// calculate norm(startPos-curv)
				curvNorm = 0.0;
				for(int i=0; i<3; i++){
					curvNorm += (startPos[i]-curv[i])*(startPos[i]-curv[i]);
				}
				curvNorm = sqrt(curvNorm);

				// check if in ROI
				angleStep = roiStep/radius;
				checkAngle = 0.0;
				checkLength = 0.0;
				inROI = true;
				inFOV = true;
				while (checkLength < length && inROI && inFOV){
					for(int i=0; i<3; i++){
						checkPos[i] = curv[i] + radius*((startPos[i]-curv[i])/curvNorm*cos(checkAngle)+startDir[i]*sin(checkAngle));
					}

					// is point in FOV and in ROI?

					// check FOV first
					if(checkPos[0] < breastFOV[0] || checkPos[0] > breastFOV[1] ||
						checkPos[1] < breastFOV[2] || checkPos[1] > breastFOV[3] ||
						checkPos[2] < breastFOV[4] || checkPos[2] > breastFOV[5]){
							inFOV = false;
					}

					// check in ROI

					if(inFOV){
						myBranch->myTree->breast->ComputeStructuredCoordinates(checkPos, myVoxel, pcoords);

						unsigned char* p =
							static_cast<unsigned char*>(myBranch->myTree->breast->GetScalarPointer(myVoxel[0],myVoxel[1],myVoxel[2]));
						bool inBreast = true;
						if(p[0] == myBranch->myTree->tissue->skin || p[0] == myBranch->myTree->tissue->bg){
							inBreast = false;
						}
						if(!inBreast){
							inROI = false;
						}
					}

					checkAngle += angleStep;
					checkLength += angleStep*radius;
				}
				// check the end point
				for(int i=0; i<3; i++){
					checkPos[i] = curv[i] + radius*((startPos[i]-curv[i])/curvNorm*cos(length/radius)+startDir[i]*sin(length/radius));
				}
				// check FOV first
				if(checkPos[0] < breastFOV[0] || checkPos[0] > breastFOV[1] ||
					checkPos[1] < breastFOV[2] || checkPos[1] > breastFOV[3] ||
					checkPos[2] < breastFOV[4] || checkPos[2] > breastFOV[5]){
						inFOV = false;
				}

				// check in ROI

				if(inFOV){
					myBranch->myTree->breast->ComputeStructuredCoordinates(checkPos, myVoxel, pcoords);

					unsigned char* p =
						static_cast<unsigned char*>(myBranch->myTree->breast->GetScalarPointer(myVoxel[0],myVoxel[1],myVoxel[2]));
					bool inBreast = true;
					if(p[0] == myBranch->myTree->tissue->skin || p[0] == myBranch->myTree->tissue->bg){
						inBreast = false;
					}
					if(!inBreast){
						inROI = false;
					}
				}
			}
			curTry += 1;
			// if valid
			if (inROI && inFOV){
				if(!foundSeg){
					foundSeg = true;
					// this is first valid segment, must be the best
					// calculate cost and set to current best

					// reduction in squared distance to arteries in ROI
					// only evaluate endPos
					density = 0.0;

					// iterate over fill voxels
					#pragma omp parallel for collapse(3) reduction(+:density)
					for(int a=fillExtent[0]; a<=fillExtent[1]; a++){
						for(int b=fillExtent[2]; b<=fillExtent[3]; b++){
							for(int c=fillExtent[4]; c<=fillExtent[5]; c++){
								double* v = static_cast<double*>(myBranch->myTree->fill->GetScalarPointer(a,b,c));
								if(v[0] > 0.0){
									double dist;
									// voxel in ROI, calculate change in distance
									// voxel location
									vtkIdType id;
									int coord[3];
									coord[0] = a;
									coord[1] = b;
									coord[2] = c;
									id = myBranch->myTree->fill->ComputePointId(coord);
									// get spatial coordinates of point
									double pos[3];
									myBranch->myTree->fill->GetPoint(id,pos);
									dist = vtkMath::Distance2BetweenPoints(checkPos, pos);
									if(dist < v[0]){
										density -= v[0] - dist;
									}
								}
							}
						}
					}

					// penalty includes direction of segment (away from preferential direction)
					// negative cost is good, dot product gives cosine of angle
					// endDir from derivative of position
					for(int i=0; i<3; i++){
						endDir[i] = -1*(startPos[i]-curv[i])/curvNorm*sin(length/radius)+startDir[i]*cos(length/radius);
					}
					// normalize
					vtkMath::Normalize(endDir);

					bestCost = densityWt*density - angleWt*vtkMath::Dot(endDir,prefDir);
					bestRadius = radius;
					for(int i=0; i<3; i++){
						bestCurv[i] = curv[i];
					}
				} else {
					// calculate segment cost, if best yet, update current best
					density = 0.0;

					// iterate over fill voxels
					#pragma omp parallel for collapse(3) reduction(+:density)
					for(int a=fillExtent[0]; a<=fillExtent[1]; a++){
						for(int b=fillExtent[2]; b<=fillExtent[3]; b++){
							for(int c=fillExtent[4]; c<=fillExtent[5]; c++){
								double* v = static_cast<double*>(myBranch->myTree->fill->GetScalarPointer(a,b,c));
								if(v[0] > 0.0){
									double dist;
									// voxel in ROI, calculate change in distance
									// voxel location
									vtkIdType id;
									int coord[3];
									coord[0] = a;
									coord[1] = b;
									coord[2] = c;
									id = myBranch->myTree->fill->ComputePointId(coord);
									// get spatial coordinates of point
									double pos[3];
									myBranch->myTree->fill->GetPoint(id,pos);
									dist = vtkMath::Distance2BetweenPoints(checkPos, pos);
									if(dist < v[0]){
										density -= v[0] - dist;
									}
								}
							}
						}
					}

					// endDir from derivative of position
					for(int i=0; i<3; i++){
						endDir[i] = -1*(startPos[i]-curv[i])/curvNorm*sin(length/radius)+startDir[i]*cos(length/radius);
					}
					// normalize
					vtkMath::Normalize(endDir);

					cost = densityWt*density - angleWt*vtkMath::Dot(endDir,prefDir);
					if (cost < bestCost){
						// found a new best segment
						bestCost = cost;
						bestRadius = radius;
						for(int i=0; i<3; i++){
							bestCurv[i] = curv[i];
						}
					}
				}
			}
		}
		if(!foundSeg){
			// couldn't find good segment, reduce length
			length = length/10.0;  // could get into infinite loop
		}
	}
	if(!foundSeg){
		// we have failed completely
		length = 0.0;
		for(int i=0; i<3; i++){
			endPos[i] = startPos[i];
			endDir[i] = startDir[i];
		}
		endRad = startRad;
	} else {
		// determined new segment, fill in variables
		curvNorm = 0.0;
		for(int i=0; i<3; i++){
			curvNorm += (startPos[i]-bestCurv[i])*(startPos[i]-bestCurv[i]);
		}
		curvNorm = sqrt(curvNorm);

		for(int i=0; i<3; i++){
			centerCurv[i] = bestCurv[i];
			endPos[i] = centerCurv[i] + bestRadius*((startPos[i]-bestCurv[i])/curvNorm*cos(length/bestRadius)+startDir[i]*sin(length/bestRadius));
		}
		radCurv = 0.0;
		for(int i=0; i<3; i++){
			radCurv += (centerCurv[i]-startPos[i])*(centerCurv[i]-startPos[i]);
		}
		radCurv = sqrt(radCurv);

		// length already set
		// endDir from derivative of position
		for(int i=0; i<3; i++){
			endDir[i] = -1*(startPos[i]-centerCurv[i])/radCurv*sin(length/bestRadius)+startDir[i]*cos(length/bestRadius);
		}
		// normalize
		vtkMath::Normalize(endDir);

		// keeping derivatives fixed to 0 for now
		endDeriv = 0.0;
		// end radius from uniform random variable
		randVal = myBranch->myTree->u01();
		endRad = minEndRad*startRad + randVal*(maxEndRad-minEndRad)*startRad;
		// shape parameters
		setShape();
		// update voxel-based visualization
		updateMap();
		// update fill
		#pragma omp parallel for collapse(3)
		for(int a=fillExtent[0]; a<=fillExtent[1]; a++){
			for(int b=fillExtent[2]; b<=fillExtent[3]; b++){
				for(int c=fillExtent[4]; c<=fillExtent[5]; c++){
					double* v = static_cast<double*>(myBranch->myTree->fill->GetScalarPointer(a,b,c));
					if(v[0] > 0.0){
						double dist;
						// voxel in ROI
						// voxel location
						vtkIdType id;
						int coord[3];
						coord[0] = a;
						coord[1] = b;
						coord[2] = c;
						id = myBranch->myTree->fill->ComputePointId(coord);
						// get spatial coordinates of point
						double pos[3];
						myBranch->myTree->fill->GetPoint(id,pos);
						dist = vtkMath::Distance2BetweenPoints(endPos, pos);
						if(dist < v[0]){
							// update minimum distance
							v[0] = dist;
						}
					}
				}
			}
		}
	}
}

void arterySeg::setShape(){
	// cubic polynomial v(0)d^3+v(1)d^2+v(2)d+v(3)
	// d = [0,length)
	shape[3] = startRad;
	shape[2] = startDeriv;
	double c = endRad-shape[2]*length-shape[3];
	shape[0] = (endDeriv-shape[2]-2*c/length)/length/length;
	shape[1] = c/length/length-shape[0]*length;
}

double arterySeg::getRadius(double t){
	// return segment radius at position t (mm)
	if(t>=0 && t<=length){
		return(shape[0]*t*t*t+shape[1]*t*t+shape[2]*t+shape[3]);
	} else {
		return(0);
	}
}

void arterySeg::updateMap(){

	const double pi = boost::math::constants::pi<double>();

	// update voxelized map of arteries
	double basis1[3];
	double basis2[3];
	double basis3[3];

	for(int i=0; i<3; i++){
		basis1[i] = startDir[i];
		basis2[i] = (centerCurv[i] - startPos[i])/radCurv;
	}

	vtkMath::Cross(basis1,basis2,basis3);

	// step size for updating, half of voxel width
	double spacing[3];
	myBranch->myTree->breast->GetSpacing(spacing);
	double step = spacing[0];
	if(spacing[1] < step){
		step = spacing[1];
	}
	if(spacing[2] < step){
		step = spacing[2];
	}
	step = step/2.0;

	// step along segment, radius
	double ls,rs;

	// step along length of segment
	ls = step;
	// step along radius of segment
	rs = step;

	// position along segment, radius and angle
	//double lpos=0.0;

	// calculate number of for loops for openMP
	int lIter = (int)(ceil(length/ls));

	#pragma omp parallel for
	for(int j=0; j<=lIter; j++){
		double lpos = j*ls;
		double currentRad = getRadius(lpos);
		double rpos = 0.0;

		double currentPos[3];
		double currentDir[3];
		double lbasis2[3];
		
		// current position
		for(int i=0; i<3; i++){
			currentPos[i] = centerCurv[i] + radCurv*(-1*cos(lpos/radCurv)*basis2[i] + sin(lpos/radCurv)*basis1[i]);
			currentDir[i] = sin(lpos/radCurv)*basis2[i] + cos(lpos/radCurv)*basis1[i];
			lbasis2[i] = (centerCurv[i] - currentPos[i])/radCurv;
		}

		// plane perpendicular to currentDir spanned by basis3 and lbasis2
		while(rpos < currentRad){
			double checkPos[3];
			int checkIdx[3];
			double pcoords[3];
			
			if(rpos < step){
				// only check current voxel assume angle = 0
				for(int i=0; i<3; i++){
					checkPos[i] = currentPos[i] + rpos*(-1*cos(0.0)*lbasis2[i] + sin(0.0)*basis3[i]);
				}
				myBranch->myTree->breast->ComputeStructuredCoordinates(checkPos, checkIdx, pcoords);
				// set voxel to artery
				unsigned char* p =
					static_cast<unsigned char*>(myBranch->myTree->breast->GetScalarPointer(checkIdx[0],checkIdx[1],checkIdx[2]));
				p[0] = myBranch->myTree->tissue->artery;
			} else {
				// angle step
				double as = step/rpos;
				double apos = 0.0;
				while(apos < 2*pi){
					for(int i=0; i<3; i++){
						checkPos[i] = currentPos[i] + rpos*(-1*cos(apos)*lbasis2[i] + sin(apos)*basis3[i]);
					}
					myBranch->myTree->breast->ComputeStructuredCoordinates(checkPos, checkIdx, pcoords);
					// set voxel to artery
					unsigned char* p =
						static_cast<unsigned char*>(myBranch->myTree->breast->GetScalarPointer(checkIdx[0],checkIdx[1],checkIdx[2]));
					p[0] = myBranch->myTree->tissue->artery;
					apos += as;
				}
			}
			rpos += rs;
		}

		lpos += ls;
	}
}