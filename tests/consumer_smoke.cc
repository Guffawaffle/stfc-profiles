#include "stfc_profiles/component.h"
#include "stfc_profiles/identity.h"

#include <iostream>
#include <stdexcept>

int main()
{
  if (stfc::profiles::ComponentVersion() != "0.3.0-dev"
      || stfc::profiles::ExtractionSourceRevision().size() != 40
      || !stfc::profiles::ValidId("0123456789abcdef0123456789abcdef"))
    throw std::runtime_error("public source-library interface mismatch");
  std::cout << "public consumer linked " << stfc::profiles::ComponentVersion() << '\n';
}
