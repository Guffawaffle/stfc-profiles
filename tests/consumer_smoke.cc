#include "stfc_profiles/component.h"
#include "stfc_profiles/legacy_contract.h"

#include <iostream>
#include <stdexcept>

int main()
{
  if (stfc::profiles::ComponentVersion() != "0.1.0-dev"
      || stfc::profiles::ExtractionSourceRevision().size() != 40
      || !stfc::profiles::legacy::ValidId("consumer_1"))
    throw std::runtime_error("public source-library interface mismatch");
  std::cout << "public consumer linked " << stfc::profiles::ComponentVersion() << '\n';
}
