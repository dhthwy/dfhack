/*
www.sourceforge.net/projects/dfhack
Copyright (c) 2009 Petr Mrázek (peterix), Kenneth Ferland (Impaler[WrG]), dorf

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "DFCommonInternal.h"
#include "../private/APIPrivate.h"
#include "modules/Materials.h"
#include "DFMemInfo.h"
#include "DFProcess.h"
#include "DFVector.h"

using namespace DFHack;

class Materials::Private
{
    public:
    APIPrivate *d;
    Process * owner;
    /*
    bool Inited;
    bool Started;
    */
};

Materials::Materials(APIPrivate * d_)
{
    d = new Private;
    d->d = d_;
    d->owner = d_->p;
}
Materials::~Materials()
{
    delete d;
}
/*
    {
LABEL_53:
      if ( a1
        || (signed int)a2 < 0
        || a2 >= (inorg_end - inorg_start) >> 2
        || (v13 = *(_DWORD *)(inorg_start + 4 * a2), !v13) )
      {
        switch ( a1 )
        {
          case 1:
            sub_40FDD0("AMBER");
            break;
          case 2:
            sub_40FDD0("CORAL");
            break;
          case 3:
            sub_40FDD0("GLASS_GREEN");
            break;
          case 4:
            sub_40FDD0("GLASS_CLEAR");
            break;
          case 5:
            sub_40FDD0("GLASS_CRYSTAL");
            break;
          case 6:
            sub_40FDD0("WATER");
            break;
          case 7:
            sub_40FDD0("COAL");
            break;
          case 8:
            sub_40FDD0("POTASH");
            break;
          case 9:
            sub_40FDD0("ASH");
            break;
          case 10:
            sub_40FDD0("PEARLASH");
            break;
          case 11:
            sub_40FDD0("LYE");
            break;
          case 12:
            sub_40FDD0("MUD");
            break;
          case 13:
            sub_40FDD0("VOMIT");
            break;
          case 14:
            sub_40FDD0("SALT");
            break;
          case 15:
            sub_40FDD0("FILTH_B");
            break;
          case 16:
            sub_40FDD0("FILTH_Y");
            break;
          case 17:
            sub_40FDD0("UNKNOWN_SUBSTANCE");
            break;
          case 18:
            sub_40FDD0("GRIME");
            break;
          default:
            sub_40A070("NONE", 4u);
            break;
        }
        result = sub_40A070("NONE", 4u);
        if ( a1 == 7 )
        {
          result = a2;
          if ( a2 )
          {
            if ( a2 == 1 )
              result = sub_40A070("CHARCOAL", 8u);
          }
          else
          {
            result = sub_40A070("COKE", 4u);
          }
        }
      }
      else
      {
        sub_40A070("INORGANIC", 9u);
        result = sub_409CA0(v13, 0, -1);
      }
    }

*/

/*
bool API::ReadInorganicMaterials (vector<t_matgloss> & inorganic)
{
    Process *p = d->owner;
    memory_info * minfo = p->getDescriptor();
    int matgloss_address = minfo->getAddress ("mat_inorganics");
    int matgloss_colors = minfo->getOffset ("material_color");
    int matgloss_stone_name_offset = minfo->getOffset("matgloss_stone_name");

    DfVector <uint32_t> p_matgloss (p, matgloss_address);

    uint32_t size = p_matgloss.getSize();
    inorganic.resize (0);
    inorganic.reserve (size);
    for (uint32_t i = 0; i < size;i++)
    {
        // read the matgloss pointer from the vector into temp
        uint32_t temp = p_matgloss[i];
        // read the string pointed at by
        t_matgloss mat;
        //cout << temp << endl;
        //fill_char_buf(mat.id, d->p->readSTLString(temp)); // reads a C string given an address
        p->readSTLString (temp, mat.id, 128);
        
        p->readSTLString (temp+matgloss_stone_name_offset, mat.name, 128);
        mat.fore = (uint8_t) p->readWord (temp + matgloss_colors);
        mat.back = (uint8_t) p->readWord (temp + matgloss_colors + 2);
        mat.bright = (uint8_t) p->readWord (temp + matgloss_colors + 4);
        
        inorganic.push_back (mat);
    }
    return true;
}
*/



// good for now
inline bool ReadNamesOnly(Process* p, uint32_t address, vector<t_matgloss> & names)
{
    DfVector <uint32_t> p_matgloss (p, address);
    uint32_t size = p_matgloss.size();
    names.clear();
    names.reserve (size);
    for (uint32_t i = 0; i < size;i++)
    {
        t_matgloss mat;
        p->readSTLString (p_matgloss[i], mat.id, 128);
        names.push_back(mat);
    }
    return true;
}

bool Materials::ReadInorganicMaterials (void)
{
    return ReadNamesOnly(d->owner, d->owner->getDescriptor()->getAddress ("mat_inorganics"), inorganic );
}

bool Materials::ReadOrganicMaterials (void)
{
    return ReadNamesOnly(d->owner, d->owner->getDescriptor()->getAddress ("mat_organics_all"), organic );
}

bool Materials::ReadWoodMaterials (void)
{
    return ReadNamesOnly(d->owner, d->owner->getDescriptor()->getAddress ("mat_organics_trees"), tree );
}

bool Materials::ReadPlantMaterials (void)
{
    return ReadNamesOnly(d->owner, d->owner->getDescriptor()->getAddress ("mat_organics_plants"), plant );
}

bool Materials::ReadCreatureTypes (void)
{
    return ReadNamesOnly(d->owner, d->owner->getDescriptor()->getAddress ("creature_type_vector"), race );
    return true;
}

bool Materials::ReadDescriptorColors (void)
{
	Process * p = d->owner;
	DfVector <uint32_t> p_colors (p, p->getDescriptor()->getAddress ("descriptor_colors_vector"));
	uint32_t size = p_colors.size();
	
	color.clear();
	if(size == 0)
		return false;
	color.reserve(size);
	for (uint32_t i = 0; i < size;i++)
	{
		t_descriptor_color col;
		p->readSTLString (p_colors[i] + p->getDescriptor()->getOffset ("descriptor_rawname"), col.id, 128);
		p->readSTLString (p_colors[i] + p->getDescriptor()->getOffset ("descriptor_name"), col.name, 128);
		col.r = p->readFloat( p_colors[i] + p->getDescriptor()->getOffset ("descriptor_color_r") );
		col.v = p->readFloat( p_colors[i] + p->getDescriptor()->getOffset ("descriptor_color_v") );
		col.b = p->readFloat( p_colors[i] + p->getDescriptor()->getOffset ("descriptor_color_b") );
		color.push_back(col);
	}
	return true;
}

bool Materials::ReadCreatureTypesEx (void)
{
    Process *p = d->owner;
    memory_info *mem = d->owner->getDescriptor();
    DfVector <uint32_t> p_races (p, mem->getAddress ("creature_type_vector"));
    uint32_t castes_vector_offset = mem->getOffset ("creature_type_caste_vector");
    uint32_t sizeof_string = mem->getHexValue ("sizeof_string");
    uint32_t size = p_races.size();
    uint32_t sizecas = 0;
    uint32_t tile_offset = mem->getOffset ("creature_tile");
    uint32_t tile_color_offset = mem->getOffset ("creature_tile_color");
    raceEx.clear();
    raceEx.reserve (size);
    for (uint32_t i = 0; i < size;i++)
    {
        t_creaturetype mat;
        p->readSTLString (p_races[i], mat.rawname, sizeof(mat.rawname));
        DfVector <uint32_t> p_castes(p, p_races[i] + castes_vector_offset);
        sizecas = p_castes.size();
        for (uint32_t j = 0; j < sizecas;j++)
        {
            t_creaturecaste caste;
            uint32_t caste_start = p_castes[j];
            p->readSTLString (caste_start, caste.rawname, sizeof(caste.rawname));
            p->readSTLString (caste_start + sizeof_string, caste.singular, sizeof(caste.singular));
            p->readSTLString (caste_start + 2 * sizeof_string, caste.plural, sizeof(caste.plural));
            p->readSTLString (caste_start + 3 * sizeof_string, caste.adjective, sizeof(caste.adjective));
            mat.castes.push_back(caste);
        }
	mat.tile_character = p->readByte( p_races[i] + tile_offset );
        mat.tilecolor.fore = p->readWord( p_races[i] + tile_color_offset );
        mat.tilecolor.back = p->readWord( p_races[i] + tile_color_offset + 2 );
        mat.tilecolor.bright = p->readWord( p_races[i] + tile_color_offset + 4 );
        raceEx.push_back(mat);
    }
    return true;
}

void Materials::ReadAllMaterials(void)
{
	this->ReadInorganicMaterials();
        this->ReadOrganicMaterials();
        this->ReadWoodMaterials();
        this->ReadPlantMaterials();
        this->ReadCreatureTypes();
        this->ReadCreatureTypesEx();
	this->ReadDescriptorColors();
}

std::string Materials::getDescription(t_material & mat)
{
	std::string out;

	switch(mat.itemType)
	{
		case 0:
			if(mat.index>=0)
			{
				if(mat.index<=this->inorganic.size())
				{
					out.append(this->inorganic[mat.index].id);
					out.append(" bar");
				}
				else
					out = "invalid bar";
			}
			else
				out = "any metal bar";
			break;
		case 1:
			out = "cut gem";
			break;
		case 2:
			out = "block";
			break;
		case 3:
			switch(mat.subType)
			{
				case 3: out = "raw green glass"; break;
				case 4: out = "raw clear glass"; break;
				case 5: out = "raw crystal glass"; break;
				default: out = "raw gems"; break;
			}
			break;
		case 4:
			out = "raw stone";
			break;
		case 5:
			out = "wood log";
			break;
		case 24:
			out = "weapon?";
			break;
		case 26:
			out = "footwear";
			break;
		case 28:
			out = "headwear";
			break;
		case 54:
			out = "leather";
			break;
		case 57:
			out = "cloth";
			break;
		case 71:
			out = "food";
			break;
		default:
			out = "unknown";
			break;
	}
	return out;
}

